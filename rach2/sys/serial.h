#ifndef _SERIAL_H_
#define _SERIAL_H_

#include "lib/kint.h"

/* COM1 port base address */
#define SERIAL_COM1 0x3F8

/** Detect and configure COM1 for debug output. */
void serial_init(void);

/**
 * Write one byte to the port.
 *
 * The transmit wait gives up after a fixed number of polls rather than
 * spinning for ever, so a UART that stopped draining costs a bounded delay
 * per byte and not the machine. A machine with no COM1 discards the byte.
 *
 * This is the only way out of the port, and it has no formatting, no copy of
 * what it sent, and no tap: what reaches the port is decided by `klog` and by
 * the console calls beside it, which keep the machine's own copy in the log's
 * ring. Nothing else writes here.
 */
void serial_out(char c);

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
