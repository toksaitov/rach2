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
 * what it sent, and no tap: what reaches the port is decided by `klog`, which
 * keeps the machine's own copy in the log's ring, and by the console calls
 * beside it, whose output is not part of that log. Nothing else writes here.
 */
void serial_out(char c);

/**
 * Whether a received byte is waiting.
 *
 * Returns zero when none is, and when no COM1 was detected.
 */
int serial_data_available(void);

/**
 * Read a byte from the port without waiting.
 *
 * Returns the byte, or -1 when none is ready and when no COM1 was detected.
 * There is no blocking read: a kernel that waits on a port nobody is typing at
 * is a kernel that has stopped.
 */
int serial_try_getc(void);

#endif
