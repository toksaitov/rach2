#ifndef _SERIAL_H_
#define _SERIAL_H_

#include "lib/kint.h"

/* COM1 port base address */
#define SERIAL_COM1 0x3F8

/** Detect and configure COM1 for debug output. */
void serial_init(void);

/* The receive path's counters, for `/proc/console`. */
struct serial_rx_counters {
    uint32_t bytes;        /* bytes taken from the UART */
    uint32_t overruns;     /* times the line status reported an overrun */
    uint32_t irq_services; /* services run from the port's interrupt */
    uint32_t irq_bytes;    /* bytes those services took */
    uint32_t ring_full;    /* services that stopped at a full ring */
    uint32_t ring_max;     /* the most the ring held */
    uint32_t polled_bytes; /* bytes taken by the kernel's own polls */
};

/**
 * Move what the UART holds into the receive ring, at most `budget` bytes.
 *
 * Run from the port's interrupt and from the timer tick, so bytes are taken
 * whether or not the interrupt is enabled; the ring holds them until a
 * reader, a pump or the debug shell takes them. A full ring stops the
 * service and masks the receive interrupt: the bytes stay in the UART, whose
 * full FIFO is what tells an emulator's sender to wait, and a real sender
 * overruns it, which the line status reports and `overruns` counts. Taking
 * from the ring unmasks the interrupt again. Returns the count moved.
 */
int serial_rx_service(int budget);

/** Enable the port's receive interrupt; `serial_rx_service` runs on it. */
void serial_enable_rx_irq(void);

/**
 * Register what runs after the interrupt's service, with interrupts off:
 * the console's wake of a parked reader or its drain into the discipline.
 */
void serial_set_rx_notify(void (*notify)(void));

/** Take a byte from the ring for a pump, or -1; not counted as a poll. */
int serial_rx_take(void);

/** How many bytes the ring holds. */
uint32_t serial_rx_pending(void);

/** The offset in the ring of the first byte in `set` (zeros ignored), or -1. */
int serial_rx_find(const unsigned char *set, int n);

/** Drop `count` bytes from the ring's head. */
void serial_rx_discard(int count);

/** Discard the ring and the UART's receiver: a flush of the console's input. */
void serial_rx_flush(void);

/** Copy the receive counters out. */
void serial_rx_counters(struct serial_rx_counters *out);

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
 * Whether a received byte is waiting in the ring.
 *
 * Returns zero when none is, and when no COM1 was detected.
 */
int serial_data_available(void);

/**
 * Take a byte from the ring without waiting, for the kernel's own polls.
 *
 * Returns the byte, or -1 when none is ready and when no COM1 was detected.
 * There is no blocking read: a kernel that waits on a port nobody is typing at
 * is a kernel that has stopped. The debug shell and the boot diagnostic hold
 * read here, and the count of what they took is `polled_bytes`.
 */
int serial_try_getc(void);

#endif
