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

/*
 * Check if data is available to read from serial port.
 * Returns non-zero if data is ready, 0 otherwise.
 * This is a non-blocking check.
 */
int serial_data_available(void);

/*
 * Read a single character from serial port (blocking).
 * Waits until data is available, then returns the character.
 */
char serial_getc(void);

/*
 * Try to read a character from serial port (non-blocking).
 * Returns the character if available, or -1 if no data ready.
 */
int serial_try_getc(void);

#endif
