#ifndef _SERIAL_H_
#define _SERIAL_H_

#include "lib/kint.h"

/* COM1 port base address */
#define SERIAL_COM1 0x3F8

/** Detect and configure COM1 for debug output. */
void serial_init(void);

/*
 * Write a single character to the serial port. Spins until the transmit
 * holding register is empty, then sends. A no-op if no COM1 was detected.
 */
void serial_putc(char c);

/*
 * Repeat every character written here to `tap` as well, or stop repeating when
 * `tap` is NULL. The tap is offered the character before the port is checked,
 * so it still sees the stream on a machine that has no COM1, which is the case
 * it exists for; see network/netconsole.c, the only caller. It runs from
 * interrupt handlers and from the panic path, so it must not block, allocate,
 * or write to the serial port itself.
 */
void serial_set_tap(void (*tap)(char c));

/*
 * Write a null-terminated string to the serial port. Each LF is sent as
 * CR LF.
 */
void serial_puts(const char *str);

/*
 * Formatted output to the serial port. Supports %d (decimal), %x/%X (hex),
 * %s (string), and %% (literal percent).
 */
void serial_printf(const char *format, ...);

/*
 * Non-blocking check for received data. Returns non-zero if a byte is ready,
 * 0 otherwise or if no COM1 was detected.
 */
int serial_data_available(void);

/*
 * Report whether a COM1 UART was detected during init. Returns 1 if it
 * exists, 0 otherwise (such as real hardware without a serial port). Callers
 * use this to avoid spinning on a non-existent port.
 */
int serial_port_exists(void);

/*
 * Read a single character from the serial port, blocking until data is
 * available.
 */
char serial_getc(void);

/*
 * Read a character from the serial port without blocking. Returns the byte
 * if one is ready, or -1 if none is.
 */
int serial_try_getc(void);

#endif
