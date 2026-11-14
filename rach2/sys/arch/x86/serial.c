/*
 * serial.c: the PC's serial console, a 16550 UART at COM1, as serial.h
 * describes it: a polled transmit, and a receive ring that the port's
 * interrupt and the tick fill, or a line attached in the UART's place.
 */
#include "portio.h"
#include "../arch.h"
#include "../../serial.h"

/* COM1 port base address */
#define SERIAL_COM1 0x3F8
#include "../../kio.h"
#include "isr.h"
#include "pic.h"

/* Serial port register offsets from base address */
#define SERIAL_DATA        0 /* Data register (R/W) */
#define SERIAL_INTR_ENABLE 1 /* Interrupt enable register */
#define SERIAL_FIFO_CTRL   2 /* FIFO control register */
#define SERIAL_LINE_CTRL   3 /* Line control register */
#define SERIAL_MODEM_CTRL  4 /* Modem control register */
#define SERIAL_LINE_STATUS 5 /* Line status register */
#define SERIAL_SCRATCH     7 /* Scratch register (for detection) */

/* Line status register bits */
#define SERIAL_LSR_DR   0x01 /* Data ready (received data available) */
#define SERIAL_LSR_OE   0x02 /* Overrun: a byte arrived on a full receiver */
#define SERIAL_LSR_THRE 0x20 /* Transmit holding register empty */

/* Interrupt enable register bits */
#define SERIAL_IER_RDA 0x01 /* Received data available */

#define SERIAL_IRQ    4
#define SERIAL_VECTOR 36

/*
 * The receive ring: what the UART's interrupt or the tick took from the
 * port and nobody has read yet. 4096 bytes is 350 ms of input at 115200
 * baud, and the discipline's own queue behind it is as large again, so a
 * reader that is not scheduled for a third of a second loses nothing. A
 * power of two, for the mask.
 */
#define SERIAL_RX_SIZE 4096
static unsigned char _rx_ring[SERIAL_RX_SIZE];
static uint32_t _rx_head; /* monotonic; slot = head & (SIZE - 1) */
static uint32_t _rx_tail;
static struct serial_rx_counters _rx;
static void (*_rx_notify)(void);
static int _rx_irq_wanted; /* the receive interrupt was enabled */
static int _rx_irq_masked; /* and is off from a full ring to a half-empty one */
/* The handler stopped at its pass limit with a byte still in the FIFO, so
 * IRQ 4's line stays high and raises no edge until a reading empties it. */
static int _rx_line_high;
/* The ring position before which `serial_rx_lookahead` has given every
 * byte; monotonic, as the head and tail are. */
static uint32_t _rx_looked;
/*
 * Where `serial_rx_find`'s last scan for a set of three stopped, and the
 * set: the bytes from the head to there hold none of the set, so a caller
 * that asks again while the ring drains scans only what arrived since, and
 * a throttled reader asking once per byte does not walk the ring each time.
 */
static uint32_t _rx_scanned;
static unsigned char _rx_scan_set[3];

/* Line control register bits */
#define SERIAL_LCR_DLAB 0x80 /* Divisor latch access bit */

/*
 * Set once `serial_init` finds a UART at COM1. On a machine without one the
 * port reads 0xFF, a line status that says the transmitter is empty and a
 * byte is waiting, so every byte written would go nowhere and the receive
 * service would fill the ring with 0xFF; while the flag is clear the entries
 * that would reach the port return at once instead.
 */
static int _serial_port_exists = 0;

/*
 * The line that carries the console beside the UART or in its place
 * (`serial_line_attach`), or NULL. `_line_input` says the ring is the
 * line's, `_line_uart` that the UART still sends, and `_line_waits` that a
 * feed found the ring too full, so that the line is told when it has room.
 * The pointer is read without a lock by a processor that writes a report;
 * the rest are written under the console's lock.
 */
static const struct serial_line *volatile _line;
static int _line_input;
static int _line_uart;
static int _line_waits;

/** Whether the ring has a source: the UART, or a line that feeds it. */
static int _rx_source(void)
{
    return _serial_port_exists || _line_input;
}

/**
 * Read the line status, counting an overrun. Every read of the line status
 * goes through here, because reading it clears the overrun bit: a transmit
 * poll that read the register directly would clear the evidence the receive
 * side counts.
 */
static uint8_t _serial_lsr(void)
{
    uint8_t lsr = inb(SERIAL_COM1 + SERIAL_LINE_STATUS);

    if (lsr & SERIAL_LSR_OE) {
        _rx.overruns++;
    }
    return lsr;
}

/** Whether the transmit holding register can take a byte. */
static int _serial_is_transmit_empty(void)
{
    return _serial_lsr() & SERIAL_LSR_THRE;
}

/** The bytes the receive ring holds. */
static uint32_t _rx_count(void)
{
    return _rx_tail - _rx_head;
}

/** Take `n` bytes, no more than the ring holds, from the ring's head, and
 * bring `_rx_looked` and `_rx_scanned` up to the head when it passes them,
 * so that each stays within the ring's bytes and their distances from the
 * head never wrap. The caller holds the console's lock. */
static void _rx_advance(uint32_t n)
{
    _rx_head += n;
    if (_rx_looked - _rx_head > _rx_count()) {
        _rx_looked = _rx_head;
    }
    if (_rx_scanned - _rx_head > _rx_count()) {
        _rx_scanned = _rx_head;
    }
}

/**
 * Probe for a COM1 UART using its scratch register.
 *
 * Writes two distinct test bytes to the scratch register (offset 7) and reads
 * each back. A UART keeps the value; a port nothing answers reads 0xFF.
 *
 * @return 1 if both bytes read back, 0 otherwise.
 */
static int _serial_detect_port(void)
{
    uint16_t port = SERIAL_COM1;
    uint8_t scratch;

    outb(port + SERIAL_SCRATCH, 0x5A);
    scratch = inb(port + SERIAL_SCRATCH);
    if (scratch != 0x5A) {
        return 0;
    }

    outb(port + SERIAL_SCRATCH, 0xA5);
    scratch = inb(port + SERIAL_SCRATCH);
    if (scratch != 0xA5) {
        return 0;
    }

    return 1;
}

/**
 * COM1 is set to 115200 baud, a divisor of 1 on the UART's 1.8432 MHz clock
 * over 16, with eight data bits, no parity and one stop bit, and the FIFO
 * on with a 14-byte receive threshold. OUT2 connects the UART's interrupt to
 * its ISA line. Without a UART nothing past the probe is written to the
 * port.
 */
void serial_init(void)
{
    uint16_t port = SERIAL_COM1;

    _serial_port_exists = _serial_detect_port();

    if (!_serial_port_exists) {
        return;
    }

    outb(port + SERIAL_INTR_ENABLE, 0x00);          /* Disable interrupts */
    outb(port + SERIAL_LINE_CTRL, SERIAL_LCR_DLAB); /* Enable DLAB */
    outb(port + SERIAL_DATA, 0x01);                 /* Divisor low (115200 baud) */
    outb(port + SERIAL_INTR_ENABLE, 0x00);          /* Divisor high */
    outb(port + SERIAL_LINE_CTRL, 0x03);            /* 8N1, disable DLAB */
    outb(port + SERIAL_FIFO_CTRL, 0xC7);            /* Enable FIFO, 14-byte threshold */
    outb(port + SERIAL_MODEM_CTRL, 0x0B);           /* DTR, RTS, OUT2 */
}

/*
 * How long a transmit waits for the UART before the byte is dropped.
 *
 * A UART that is not draining makes the wait below never end, and a kernel
 * that hangs while reporting a fault reports nothing. A UART that drains
 * slowly is no fault: QEMU's keeps a byte in its FIFO while the program that
 * reads its console socket falls behind, and with one-byte writes the socket
 * is full after a few hundred unread bytes. A test runner that serves many
 * machines from one loop can fall seconds behind on a loaded host, and a byte
 * dropped then is lost from the record a test reads. A hardware UART empties
 * its 16-byte FIFO in 1.4 ms at 115200 baud. So a byte waits up to
 * SERIAL_TX_WAIT_MS, and a port that misses that is taken as stuck: until it
 * takes a byte again, each byte waits only SERIAL_TX_STUCK_MS, so that a dead
 * port costs one long wait and then a short one per byte.
 *
 * The waits are counted in cycles of the time-stamp counter at
 * SERIAL_TX_CYCLES_PER_MS, faster than any processor's counter runs, so that
 * no wait is shorter than its milliseconds. A count of polls would not do: a
 * poll takes some 100 ns under QEMU's emulation and over 1 us on a PC.
 */
#define SERIAL_TX_WAIT_MS       30000UL
#define SERIAL_TX_STUCK_MS      2UL
#define SERIAL_TX_CYCLES_PER_MS 5000000UL

static int _serial_stuck; /* the last byte timed out; reset by one sent */

void serial_out(char c)
{
    const struct serial_line *line = _line;
    uint64_t start, limit;

    if (line) {
        line->out(c);
        if (!_line_uart) {
            return;
        }
    }
    if (!_serial_port_exists) {
        return;
    }
    if (!_serial_is_transmit_empty()) {
        limit = (uint64_t)(_serial_stuck ? SERIAL_TX_STUCK_MS : SERIAL_TX_WAIT_MS) *
                SERIAL_TX_CYCLES_PER_MS;
        start = arch_cycles();
        while (!_serial_is_transmit_empty()) {
            if (arch_cycles() - start >= limit) {
                _serial_stuck = 1;
                return;
            }
        }
    }
    _serial_stuck = 0;
    outb(SERIAL_COM1 + SERIAL_DATA, c);
}

int serial_rx_service(int budget)
{
    uint32_t flags;
    int moved = 0;

    if (!_serial_port_exists || _line_input) {
        return 0;
    }
    flags = console_lock();
    while (moved < budget && _rx_count() < SERIAL_RX_SIZE) {
        unsigned char c;

        if (!(_serial_lsr() & SERIAL_LSR_DR)) {
            _rx_line_high = 0;
            break;
        }
        c = inb(SERIAL_COM1 + SERIAL_DATA);

        _rx.bytes++;
        moved++;
        _rx_ring[_rx_tail & (SERIAL_RX_SIZE - 1)] = c;
        _rx_tail++;
        if (_rx_count() > _rx.ring_max) {
            _rx.ring_max = _rx_count();
        }
        if (_rx_count() == SERIAL_RX_SIZE) {
            _rx.ring_full++;
            if (_rx_irq_wanted && !_rx_irq_masked) {
                outb(SERIAL_COM1 + SERIAL_INTR_ENABLE, 0x00);
                _rx_irq_masked = 1;
            }
            break;
        }
    }
    console_unlock(flags);
    return moved;
}

/* The passes of `_serial_rx_handler` before it gives up on a port whose
 * receive line stays high. A pass moves up to 128 bytes from the UART into
 * the ring, each through `serial_rx_service`: 64 that the handler reads and
 * 64 more that the notify's console service reads, so 256 passes move up
 * to 32 KiB, more than the ring and the console's discipline together
 * take, and a full ring masks the interrupt first. A console that
 * discards COM1's input, as one in a raw keyboard mode does, empties the
 * ring at each pass, so its flood is bounded by the passes alone; the
 * handler then leaves the line high (`serial_rx_line_high`), and the
 * tick's reading every fifth of a second takes what is left. Linux's
 * PASS_LIMIT of 512 bounds its handler the same way
 * (drivers/tty/serial/8250/8250_core.c). */
#define SERIAL_RX_PASSES 256

/**
 * The port's interrupt: passes of a service of at most 64 bytes, then the
 * notify when the ring holds any, until the receive line drops. IRQ 4 is
 * edge-triggered on the 8259 and the I/O APIC, so a handler that left
 * LSR.DR set would see no other interrupt from the port; the loop ends
 * when the FIFO is empty, which drops the line, or when a full ring has
 * masked the interrupt, as Linux's serial8250_interrupt loops until no
 * port has work (drivers/tty/serial/8250/8250_core.c:68-110). A byte that
 * arrives while the handler runs is taken in the same interrupt, and the
 * 64-byte passes let the notify move the ring along between them.
 */
static void _serial_rx_handler(isr_params_t *params)
{
    int pass;

    (void)params;
    if (_line_input) {
        return; /* the ring is a line's, and the UART's interrupt is off */
    }
    _rx.irq_services++;
    for (pass = 0; pass < SERIAL_RX_PASSES; pass++) {
        _rx.irq_bytes += (uint32_t)serial_rx_service(64);
        if (_rx_notify && _rx_count() > 0) {
            _rx_notify();
        }
        if (_rx_irq_masked || !(_serial_lsr() & SERIAL_LSR_DR)) {
            return;
        }
    }
    _rx_line_high = 1;
}

void serial_enable_rx_irq(void)
{
    if (!_serial_port_exists) {
        return;
    }
    set_isr_handler(SERIAL_VECTOR, &_serial_rx_handler, "com1");
    pic_unmask_irq(SERIAL_IRQ);
    _rx_irq_wanted = 1;
    if (!_line_input) {
        outb(SERIAL_COM1 + SERIAL_INTR_ENABLE, SERIAL_IER_RDA);
    }
}

int serial_rx_irq_on(void)
{
    return _line_input || _rx_irq_wanted;
}

int serial_rx_line_high(void)
{
    return !_line_input && _rx_line_high;
}

void serial_set_rx_notify(void (*notify)(void))
{
    _rx_notify = notify;
}

int serial_data_available(void)
{
    return _rx_source() && _rx_count() > 0;
}

/**
 * After bytes left the ring, with the console's lock held: turn the UART's
 * receive interrupt back on when a full ring masked it and the ring is
 * below half, and return the line to tell that the ring has room, when one
 * waits for it and the ring is below half or `emptied`, or NULL. The caller
 * calls the line's `rx_room` once it has let the lock go.
 */
static const struct serial_line *_rx_room(int emptied)
{
    if (_rx_irq_masked && !_line_input && (emptied || _rx_count() < SERIAL_RX_SIZE / 2)) {
        outb(SERIAL_COM1 + SERIAL_INTR_ENABLE, SERIAL_IER_RDA);
        _rx_irq_masked = 0;
    }
    if (_line_input && _line_waits && (emptied || _rx_count() < SERIAL_RX_SIZE / 2)) {
        _line_waits = 0;
        return _line;
    }
    return NULL;
}

uint32_t serial_rx_pending(void)
{
    return _rx_count();
}

int serial_rx_take(void)
{
    const struct serial_line *room;
    uint32_t flags;
    int c = -1;

    if (!_rx_source()) {
        return -1;
    }
    flags = console_lock();
    if (_rx_count() > 0) {
        c = _rx_ring[_rx_head & (SERIAL_RX_SIZE - 1)];
        _rx_advance(1);
    }
    room = _rx_room(0);
    console_unlock(flags);
    if (room) {
        room->rx_room();
    }
    return c;
}

int serial_try_getc(void)
{
    int c = serial_rx_take();

    if (c >= 0) {
        _rx.polled_bytes++;
    }
    return c;
}

int serial_rx_find(const unsigned char *set, int n)
{
    uint32_t flags = console_lock();
    uint32_t i;
    int found = -1;
    int same = n == 3 && set[0] == _rx_scan_set[0] && set[1] == _rx_scan_set[1] &&
               set[2] == _rx_scan_set[2];

    i = same && _rx_scanned - _rx_head <= _rx_count() ? _rx_scanned - _rx_head : 0;
    for (; i < _rx_count() && found < 0; i++) {
        unsigned char c = _rx_ring[(_rx_head + i) & (SERIAL_RX_SIZE - 1)];
        int k;

        for (k = 0; k < n; k++) {
            if (set[k] != 0 && c == set[k]) {
                found = (int)i;
                break;
            }
        }
    }
    if (n == 3) {
        _rx_scan_set[0] = set[0];
        _rx_scan_set[1] = set[1];
        _rx_scan_set[2] = set[2];
        _rx_scanned = found < 0 ? _rx_tail : _rx_head + (uint32_t)found;
    }
    console_unlock(flags);
    return found;
}

/** The bytes from the head that `serial_rx_lookahead` has given, which
 * `_rx_advance` keeps within the ring's bytes. */
static uint32_t _rx_looked_count(void)
{
    uint32_t d = _rx_looked - _rx_head;

    return d <= _rx_count() ? d : 0;
}

int serial_rx_lookahead(unsigned char *buf, int max)
{
    uint32_t flags = console_lock();
    uint32_t at = _rx_head + _rx_looked_count();
    int n = 0;

    while (at != _rx_tail && n < max) {
        buf[n++] = _rx_ring[at & (SERIAL_RX_SIZE - 1)];
        at++;
    }
    _rx_looked = at;
    console_unlock(flags);
    return n;
}

int serial_rx_looked(void)
{
    return _rx_looked_count() > 0;
}

void serial_rx_discard(int count)
{
    const struct serial_line *room = NULL;
    uint32_t flags = console_lock();

    if ((uint32_t)count > _rx_count()) {
        count = (int)_rx_count();
    }
    _rx_advance((uint32_t)count);
    if (_line_input) {
        room = _rx_room(0);
    }
    console_unlock(flags);
    if (room) {
        room->rx_room();
    }
}

void serial_rx_flush(void)
{
    const struct serial_line *line;
    uint32_t flags = console_lock();

    _rx_advance(_rx_count());
    line = _line_input ? _line : NULL;
    if (_serial_port_exists && !line) {
        /* The bytes a full ring left in the UART are input too. */
        outb(SERIAL_COM1 + SERIAL_FIFO_CTRL, 0xC7 | 0x02); /* FIFO on, receiver cleared */
    }
    (void)_rx_room(1);
    console_unlock(flags);
    if (line) {
        /* The bytes a full ring left with the line are input too. */
        line->rx_discard();
    }
}

void serial_rx_counters(struct serial_rx_counters *out)
{
    uint32_t flags = console_lock();

    *out = _rx;
    console_unlock(flags);
}

void serial_line_attach(const struct serial_line *line, int input, int uart)
{
    uint32_t flags = console_lock();

    _line_uart = uart;
    _line_waits = 0;
    _line_input = input;
    if (_serial_port_exists && _rx_irq_wanted) {
        outb(SERIAL_COM1 + SERIAL_INTR_ENABLE, input ? 0x00 : SERIAL_IER_RDA);
        _rx_irq_masked = 0;
    }
    _line = line;
    console_unlock(flags);
}

void serial_line_detach(const struct serial_line *line)
{
    uint32_t flags = console_lock();

    if (_line == line) {
        _line = NULL;
        _line_input = 0;
        _line_uart = 0;
        _line_waits = 0;
        if (_serial_port_exists && _rx_irq_wanted) {
            outb(SERIAL_COM1 + SERIAL_INTR_ENABLE, SERIAL_IER_RDA);
            _rx_irq_masked = 0;
        }
    }
    console_unlock(flags);
}

int serial_rx_feed(const unsigned char *buf, int n)
{
    uint32_t flags = console_lock();
    int taken = 0;

    if (!_line_input) {
        console_unlock(flags);
        return 0;
    }
    while (taken < n && _rx_count() < SERIAL_RX_SIZE) {
        _rx_ring[_rx_tail & (SERIAL_RX_SIZE - 1)] = buf[taken++];
        _rx_tail++;
    }
    _rx.bytes += (uint32_t)taken;
    if (_rx_count() > _rx.ring_max) {
        _rx.ring_max = _rx_count();
    }
    if (taken < n) {
        _rx.ring_full++;
        _line_waits = 1;
    }
    console_unlock(flags);
    return taken;
}

void serial_rx_fed(void)
{
    if (_rx_notify && _rx_count() > 0) {
        _rx.irq_services++;
        _rx_notify();
    }
}

void serial_flush(void)
{
    const struct serial_line *line = _line;

    if (line) {
        line->flush();
    }
}

int serial_uart_found(void)
{
    return _serial_port_exists;
}
