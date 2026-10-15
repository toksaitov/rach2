/*
 * serial.h: the serial console. The kernel's log and the debug shell go out
 * on it, and what it receives reaches the console's readers. A platform's
 * UART driver defines it: arch/x86/serial.c on a PC, COM1's 16550.
 */
#ifndef _SERIAL_H_
#define _SERIAL_H_

#include "lib/kint.h"

/**
 * Find the UART by its scratch register and set it to 115200 baud, eight
 * data bits, no parity and one stop bit, with its FIFO on and its interrupts
 * off. With no UART found, every other call here sends nothing and receives
 * nothing.
 */
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
 * Move what the UART holds into the receive ring, at most `budget` bytes,
 * under the console's lock (`console_lock`).
 *
 * Run from the port's interrupt and from the timer tick, so bytes are taken
 * whether or not the interrupt is enabled; the ring holds them until a
 * reader, a pump or the debug shell takes them. A full ring stops the
 * service and masks the receive interrupt: the bytes stay in the UART, whose
 * full FIFO is what tells an emulator's sender to wait, and a real sender
 * overruns it, which the line status reports and `overruns` counts. Taking
 * the ring below half full, or flushing it, unmasks the interrupt again.
 * Returns the count moved.
 */
int serial_rx_service(int budget);

/** Take the port's interrupt and enable its receive interrupt, on which
 * `serial_rx_service` runs for up to 64 bytes and then the function
 * `serial_set_rx_notify` gave, when the ring holds a byte. */
void serial_enable_rx_irq(void);

/**
 * Register what runs after the interrupt's service, with interrupts off:
 * the console's wake of a parked reader or its drain into the discipline.
 */
void serial_set_rx_notify(void (*notify)(void));

/** Take a byte from the ring for a pump, or -1 when it is empty or there is
 * no UART; not counted as a poll. Takes the console's lock. */
int serial_rx_take(void);

/** How many bytes the ring holds. */
uint32_t serial_rx_pending(void);

/**
 * The offset from the ring's head of the first byte equal to one of the `n`
 * bytes of `set`, a zero in `set` matching nothing, or -1. A search with
 * three bytes in `set` keeps how far it got, and the next search with the
 * same three starts there. Takes the console's lock.
 */
int serial_rx_find(const unsigned char *set, int n);

/** Drop `count` bytes from the ring's head, or every byte it holds when it
 * holds fewer. */
void serial_rx_discard(int count);

/** Discard the ring and the UART's receiver: a flush of the console's input. */
void serial_rx_flush(void);

/** Copy the receive counters out, under the console's lock. */
void serial_rx_counters(struct serial_rx_counters *out);

/**
 * Write one byte to the port.
 *
 * The transmit wait gives up after a fixed number of polls and drops the
 * byte, so a UART that stopped draining costs a bounded delay per byte and
 * not the machine. A machine with no UART discards the byte.
 *
 * It formats nothing and keeps no copy of what it sent. Its callers are
 * `klog`, which keeps the machine's own copy in the log's ring, and the
 * console calls beside it in klog.c, whose output is not part of that log.
 */
void serial_out(char c);

/**
 * Whether a received byte is waiting in the ring.
 *
 * Returns zero when none is, and when no UART was found.
 */
int serial_data_available(void);

/**
 * Take a byte from the ring without waiting, for the kernel's own polls.
 *
 * Returns the byte, or -1 when none is ready and when no UART was found.
 * The debug shell and the boot diagnostic's hold read here, and the count of
 * what they took is `polled_bytes`.
 */
int serial_try_getc(void);

#endif
