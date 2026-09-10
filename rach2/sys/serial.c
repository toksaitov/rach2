#include "serial.h"
#include "kio.h"
#include "isr.h"
#include "pic.h"
#include "lib/kirq.h"

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
#define SERIAL_IER_RDA  0x01 /* Received data available */

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
static int _rx_irq_masked; /* and is off while the ring is full */

/* Line control register bits */
#define SERIAL_LCR_DLAB 0x80 /* Divisor latch access bit */

/*
 * Set once a real COM1 UART is found. On hardware without COM1, port reads
 * float to 0xFF, so a wait for the port would never end; this flag makes the
 * write a no-op and the read report nothing instead.
 */
static int _serial_port_exists = 0;

/*
 * Every read of the line status goes through here, because reading it
 * clears the overrun bit: a transmit poll that read the register directly
 * would clear the evidence the receive side counts.
 */
static uint8_t _serial_lsr(void)
{
    uint8_t lsr = inb(SERIAL_COM1 + SERIAL_LINE_STATUS);

    if (lsr & SERIAL_LSR_OE) {
        _rx.overruns++;
    }
    return lsr;
}

static int _serial_is_transmit_empty(void)
{
    return _serial_lsr() & SERIAL_LSR_THRE;
}

static uint32_t _rx_count(void)
{
    return _rx_tail - _rx_head;
}

/**
 * Probe for a COM1 UART using its scratch register.
 *
 * Writes two distinct test bytes to the scratch register (offset 7) and reads
 * each back. A real UART latches the value; a floating bus returns 0xFF.
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
 * Detect and configure COM1 for 115200 baud, 8N1, FIFO enabled.
 *
 * Probes first; if no UART is present every serial entry point stays a no-op.
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
 * How many times a transmit is polled before the byte is dropped.
 *
 * A UART that is not draining makes the wait below never end, and a kernel
 * that hangs while reporting a fault reports nothing. At 115200 baud a byte
 * leaves in about 87 microseconds and this loop is far faster than that, so
 * the count is high enough that a working port never reaches it and low
 * enough that a wedged one costs a bounded delay per byte rather than the
 * machine.
 */
#define SERIAL_TX_POLLS 100000

void serial_out(char c)
{
    unsigned long spins = 0;

    if (!_serial_port_exists) {
        return;
    }
    while (!_serial_is_transmit_empty()) {
        if (++spins >= SERIAL_TX_POLLS) {
            return;
        }
    }
    outb(SERIAL_COM1 + SERIAL_DATA, c);
}

int serial_rx_service(int budget)
{
    uint32_t flags;
    int moved = 0;

    if (!_serial_port_exists) {
        return 0;
    }
    flags = kirq_save();
    while (moved < budget && _rx_count() < SERIAL_RX_SIZE && (_serial_lsr() & SERIAL_LSR_DR)) {
        unsigned char c = inb(SERIAL_COM1 + SERIAL_DATA);

        _rx.bytes++;
        moved++;
#ifdef SERIAL_RX_DROP_EVERY
        if (_rx.bytes % SERIAL_RX_DROP_EVERY == 0) {
            continue; /* the mutation: a byte dropped on purpose */
        }
#endif
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
    kirq_restore(flags);
    return moved;
}

/*
 * The port's interrupt: the FIFO is 16 bytes and the budget is four times
 * that, so a byte arriving while the handler runs is taken in the same
 * service and a flood cannot hold the handler for more than 64 bytes; what
 * is left raises the line again.
 */
static void _serial_rx_handler(isr_params_t *params)
{
    (void)params;
    _rx.irq_services++;
    serial_rx_service(64);
    if (_rx_notify && _rx_count() > 0) {
        _rx_notify();
    }
}

void serial_enable_rx_irq(void)
{
    if (!_serial_port_exists) {
        return;
    }
    set_isr_handler(SERIAL_VECTOR, &_serial_rx_handler, "com1");
    pic_unmask_irq(SERIAL_IRQ);
    _rx_irq_wanted = 1;
    outb(SERIAL_COM1 + SERIAL_INTR_ENABLE, SERIAL_IER_RDA);
}

void serial_set_rx_notify(void (*notify)(void))
{
    _rx_notify = notify;
}

int serial_data_available(void)
{
    return _serial_port_exists && _rx_count() > 0;
}

uint32_t serial_rx_pending(void)
{
    return _rx_count();
}

int serial_rx_take(void)
{
    uint32_t flags;
    int c = -1;

    if (!_serial_port_exists) {
        return -1;
    }
    flags = kirq_save();
    if (_rx_count() > 0) {
        c = _rx_ring[_rx_head & (SERIAL_RX_SIZE - 1)];
        _rx_head++;
    }
    if (_rx_irq_masked && _rx_count() < SERIAL_RX_SIZE / 2) {
        outb(SERIAL_COM1 + SERIAL_INTR_ENABLE, SERIAL_IER_RDA);
        _rx_irq_masked = 0;
    }
    kirq_restore(flags);
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

/*
 * The scan remembers how far it got with which set: the bytes between the
 * head and that position hold none of the set, so a caller that asks
 * again while the ring drains scans only what arrived since, and a
 * throttled reader asking once per byte does not walk the ring each time.
 */
static uint32_t _rx_scanned;
static unsigned char _rx_scan_set[3];

int serial_rx_find(const unsigned char *set, int n)
{
    uint32_t flags = kirq_save();
    uint32_t i;
    int found = -1;
    int same = n == 3 && set[0] == _rx_scan_set[0] && set[1] == _rx_scan_set[1] &&
               set[2] == _rx_scan_set[2];

    i = same && _rx_scanned > _rx_head && _rx_scanned <= _rx_tail ? _rx_scanned - _rx_head : 0;
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
    kirq_restore(flags);
    return found;
}

void serial_rx_discard(int count)
{
    uint32_t flags = kirq_save();

    if ((uint32_t)count > _rx_count()) {
        count = (int)_rx_count();
    }
    _rx_head += (uint32_t)count;
    kirq_restore(flags);
}

void serial_rx_flush(void)
{
    uint32_t flags = kirq_save();

    _rx_head = _rx_tail;
    if (_serial_port_exists) {
        /* The bytes a full ring left in the UART are input too. */
        outb(SERIAL_COM1 + SERIAL_FIFO_CTRL, 0xC7 | 0x02); /* FIFO on, receiver cleared */
    }
    if (_rx_irq_masked) {
        outb(SERIAL_COM1 + SERIAL_INTR_ENABLE, SERIAL_IER_RDA);
        _rx_irq_masked = 0;
    }
    kirq_restore(flags);
}

void serial_rx_counters(struct serial_rx_counters *out)
{
    uint32_t flags = kirq_save();

    *out = _rx;
    kirq_restore(flags);
}
