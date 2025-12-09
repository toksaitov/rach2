#include "serial.h"
#include "kio.h"

/* Serial port register offsets from base address */
#define SERIAL_DATA          0   /* Data register (R/W) */
#define SERIAL_INTR_ENABLE   1   /* Interrupt enable register */
#define SERIAL_FIFO_CTRL     2   /* FIFO control register */
#define SERIAL_LINE_CTRL     3   /* Line control register */
#define SERIAL_MODEM_CTRL    4   /* Modem control register */
#define SERIAL_LINE_STATUS   5   /* Line status register */

/* Line status register bits */
#define SERIAL_LSR_THRE      0x20  /* Transmit holding register empty */

/* Line control register bits */
#define SERIAL_LCR_DLAB      0x80  /* Divisor latch access bit */

static int _serial_is_transmit_empty(void)
{
    return inb(SERIAL_COM1 + SERIAL_LINE_STATUS) & SERIAL_LSR_THRE;
}

void serial_init(void)
{
    uint16_t port = SERIAL_COM1;

    outb(port + SERIAL_INTR_ENABLE, 0x00);  /* Disable interrupts */
    outb(port + SERIAL_LINE_CTRL, SERIAL_LCR_DLAB);  /* Enable DLAB */
    outb(port + SERIAL_DATA, 0x01);         /* Divisor low (115200 baud) */
    outb(port + SERIAL_INTR_ENABLE, 0x00);  /* Divisor high */
    outb(port + SERIAL_LINE_CTRL, 0x03);    /* 8N1, disable DLAB */
    outb(port + SERIAL_FIFO_CTRL, 0xC7);    /* Enable FIFO, 14-byte threshold */
    outb(port + SERIAL_MODEM_CTRL, 0x0B);   /* DTR, RTS, OUT2 */
}

void serial_putc(char c)
{
    while (!_serial_is_transmit_empty());
    outb(SERIAL_COM1 + SERIAL_DATA, c);
}

/*
 * Wrapper for serial_putc that adds CR before LF for proper line endings.
 */
static void _serial_putc_crlf(char c)
{
    if (c == '\n') serial_putc('\r');
    serial_putc(c);
}

void serial_puts(const char *str)
{
    if (str == NULL) return;
    while (*str) _serial_putc_crlf(*str++);
}

void serial_printf(const char *format, ...)
{
    char **args = (char **) &format; ++args;
    k_vprintf(_serial_putc_crlf, format, args);
}
