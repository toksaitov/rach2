#ifndef _SERIAL_H_
#define _SERIAL_H_

#include "lib/kint.h"

/* COM1 port base address */
#define SERIAL_COM1 0x3F8

/*
 * Initialize serial port COM1 for debug output.
 * Configures 115200 baud, 8N1 (8 data bits, no parity, 1 stop bit).
 */
void serial_init(void);

/*
 * Write a single character to the serial port.
 * Waits until the transmit buffer is empty before sending.
 */
void serial_putc(char c);

/*
 * Write a null-terminated string to the serial port.
 */
void serial_puts(const char *str);

/*
 * Formatted output to serial port.
 * Supports: %d (decimal), %x/%X (hex), %s (string), %% (literal %).
 */
void serial_printf(const char *format, ...);

#endif
