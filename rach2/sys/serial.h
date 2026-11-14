/*
 * serial.h: the serial console. The kernel's log and the debug shell go out
 * on it, and what it receives reaches the console's readers. A platform's
 * UART driver defines it: arch/x86/serial.c on a PC, COM1's 16550. Another
 * device may carry the console beside the UART or in its place, as a virtio
 * console's port does under `console=hvc0` (`struct serial_line`).
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

/** Whether the receive interrupt is enabled, so that the port's bytes
 * arrive without the tick's reading. */
int serial_rx_irq_on(void);

/** Whether the receive interrupt's handler stopped at its pass limit with a
 * byte still in the port, so that IRQ 4's edge-triggered line stays high
 * and no interrupt comes until a reading by `serial_rx_service` empties
 * the port, which clears it. */
int serial_rx_line_high(void);

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

/**
 * Copy into `buf`, at most `max` of them, the bytes of the ring no earlier
 * call has given, oldest first, for a reader that acts on some bytes ahead
 * of their turn (VSTOP and VSTART); the ring keeps them. Returns the count.
 * Takes the console's lock.
 */
int serial_rx_lookahead(unsigned char *buf, int max);

/** Whether the byte at the ring's head is one `serial_rx_lookahead` gave,
 * whatever took bytes from the ring since. */
int serial_rx_looked(void);

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
 * The transmit waits for the UART up to 30 seconds, long enough for a slow
 * reader of a virtual port, and drops the byte past it. A port that missed
 * that bound is waited on for 2 ms until it takes a byte again, so a UART
 * that stopped draining costs a bounded delay and not the machine. A machine
 * with no UART discards the byte.
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

/*
 * A line that carries the console beside the UART or in its place: the port
 * of a virtio console (virtio/virtio_console.c). The record outlives its
 * attachment, since a processor that writes a report without the big lock
 * may be in `out` when the line is detached; a line that has no device then
 * discards what it is given.
 *
 * out: take one byte to send. Called with interrupts on or off, from a
 *     handler, during a panic, and by a processor without the big lock.
 * flush: send what `out` took and wait, a bounded time, until the device
 *     has it, so that a reset or a power off that follows loses none.
 * rx_room: the ring has room again after a `serial_rx_feed` that it could
 *     not take whole: feed what the line held back. Not called from
 *     `serial_rx_feed`.
 * rx_discard: drop what the line holds back: a flush of the console's input.
 */
struct serial_line {
    void (*out)(char c);
    void (*flush)(void);
    void (*rx_room)(void);
    void (*rx_discard)(void);
};

/**
 * Make `line` the console's line. `serial_out` then gives it every byte,
 * and the UART too when `uart` is nonzero. With `input` nonzero the receive
 * ring holds what the line feeds (`serial_rx_feed`), the UART is no longer
 * read and its receive interrupt is off; otherwise the line's input is
 * not the console's and the UART's still is. A second attach replaces the
 * first. Under the big lock.
 */
void serial_line_attach(const struct serial_line *line, int input, int uart);

/** Give the console back to the UART when `line` is the console's line:
 * its output, its input and its receive interrupt as they were. Under the
 * big lock. */
void serial_line_detach(const struct serial_line *line);

/**
 * Put `n` bytes a line received at the ring's tail, as many as it has room
 * for, under the console's lock. When it takes fewer than `n`, the line's
 * `rx_room` is called once the ring is below half full or flushed. Returns
 * the count taken: 0 while the line is not the console's input.
 */
int serial_rx_feed(const unsigned char *buf, int n);

/** Run the function `serial_set_rx_notify` gave when the ring holds a
 * byte, as the UART's interrupt does after its service. Interrupts off,
 * under the big lock, from the line's interrupt. */
void serial_rx_fed(void);

/** Send what a line has taken and not yet sent (`flush` of the line);
 * nothing on the UART, which sends each byte as it is written. */
void serial_flush(void);

/** Whether `serial_init` found a UART at COM1. */
int serial_uart_found(void);

#endif
