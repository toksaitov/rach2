#include "serial.h"
#include "kio.h"

/* Serial port register offsets from base address */
#define SERIAL_DATA          0   /* Data register (R/W) */
#define SERIAL_INTR_ENABLE   1   /* Interrupt enable register */
#define SERIAL_FIFO_CTRL     2   /* FIFO control register */
#define SERIAL_LINE_CTRL     3   /* Line control register */
#define SERIAL_MODEM_CTRL    4   /* Modem control register */
#define SERIAL_LINE_STATUS   5   /* Line status register */
#define SERIAL_SCRATCH       7   /* Scratch register (for detection) */

/* Line status register bits */
#define SERIAL_LSR_DR        0x01  /* Data ready (received data available) */
#define SERIAL_LSR_THRE      0x20  /* Transmit holding register empty */

/* Line control register bits */
#define SERIAL_LCR_DLAB      0x80  /* Divisor latch access bit */

/*
 * Flag indicating whether a real serial port was detected.
 * On real hardware without COM1, port reads return 0xFF (floating bus),
 * which causes infinite loops in serial polling code.
 */
static int _serial_port_exists = 0;

static int _serial_is_transmit_empty(void)
{
    return inb(SERIAL_COM1 + SERIAL_LINE_STATUS) & SERIAL_LSR_THRE;
}

/*
 * Detect if a real serial port exists at COM1.
 * Uses the scratch register (0x3FF) - write test values and read back.
 * A real UART will store the value; floating bus returns 0xFF.
 */
static int _serial_detect_port(void)
{
    uint16_t port = SERIAL_COM1;
    uint8_t scratch;

    /* Test 1: Write 0x5A to scratch register and read back */
    outb(port + SERIAL_SCRATCH, 0x5A);
    scratch = inb(port + SERIAL_SCRATCH);
    if (scratch != 0x5A) {
        return 0;  /* No serial port */
    }

    /* Test 2: Write 0xA5 to scratch register and read back */
    outb(port + SERIAL_SCRATCH, 0xA5);
    scratch = inb(port + SERIAL_SCRATCH);
    if (scratch != 0xA5) {
        return 0;  /* No serial port */
    }

    return 1;  /* Serial port detected */
}

void serial_init(void)
{
    uint16_t port = SERIAL_COM1;

    /* Detect if serial port exists before initializing */
    _serial_port_exists = _serial_detect_port();

    if (!_serial_port_exists) {
        return;  /* No serial port - skip initialization */
    }

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
    if (!_serial_port_exists) {
        return;  /* No serial port - discard output */
    }
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

int serial_data_available(void)
{
    /* If no serial port exists, never report data available */
    if (!_serial_port_exists) {
        return 0;
    }
    return inb(SERIAL_COM1 + SERIAL_LINE_STATUS) & SERIAL_LSR_DR;
}

int serial_port_exists(void)
{
    return _serial_port_exists;
}

char serial_getc(void)
{
    while (!serial_data_available());
    return inb(SERIAL_COM1 + SERIAL_DATA);
}

int serial_try_getc(void)
{
    if (serial_data_available()) {
        return (int)(unsigned char)inb(SERIAL_COM1 + SERIAL_DATA);
    }
    return -1;
}
