/**
 * Host test of the console serial port, arch/x86/serial.c: the receive
 * ring's look ahead cursor at counters near the 32-bit wrap and at a cursor
 * the head has passed by far, and the transmit's reads of the line status
 * for each load of the FIFO.
 *
 * host-deps:
 *
 * The ring's rows set its counters and bytes directly, as the port's
 * interrupt leaves them, and read them back through serial.h's calls. The
 * transmit's rows run against a model of the UART's registers that counts
 * the reads of the line status and the bytes written.
 */
extern int printf(const char *format, ...);

#include "arch/x86/serial.c"

static int failures;

#define CHECK(test, detail)                              \
    do {                                                 \
        if (!(test)) {                                   \
            printf("serial-test: fail: %s\n", (detail)); \
            failures++;                                  \
        }                                                \
    } while (0)

/* The model UART: whether its FIFOs are on, the line status reads that
 * still find the transmitter busy, and the counts of status reads and of
 * bytes written to the data port. */
static int uart_fifo;
static int uart_busy_reads;
static int uart_status_reads;
static int uart_bytes;
static uint8_t uart_scratch;

/** Count a byte written to the data port and keep one written to the
 * scratch register; ignore the other registers. */
void outb(uint16_t port, uint8_t val)
{
    if (port == SERIAL_COM1 + SERIAL_DATA) {
        uart_bytes++;
    }
    if (port == SERIAL_COM1 + SERIAL_SCRATCH) {
        uart_scratch = val;
    }
}

/** The model's registers: the interrupt identification shows the FIFOs
 * when they are on, the scratch register reads back what the probe wrote,
 * and the line status shows the transmitter empty and no byte ready once
 * `uart_busy_reads` reads have found it busy. */
uint8_t inb(uint16_t port)
{
    if (port == SERIAL_COM1 + SERIAL_INTR_ID) {
        return uart_fifo ? 0xC1 : 0x01;
    }
    if (port == SERIAL_COM1 + SERIAL_SCRATCH) {
        return uart_scratch;
    }
    if (port == SERIAL_COM1 + SERIAL_LINE_STATUS) {
        uart_status_reads++;
        if (uart_busy_reads > 0) {
            uart_busy_reads--;
            return 0;
        }
        return SERIAL_LSR_THRE;
    }
    return 0;
}

/** 0: the rows run alone, with nothing to lock out. */
uint32_t console_lock(void)
{
    return 0;
}

/** Nothing, as `console_lock` took nothing. */
void console_unlock(uint32_t flags)
{
    (void)flags;
}

/** 0: no row waits on the transmitter. */
uint64_t arch_cycles(void)
{
    return 0;
}

/** Nothing: no row enables the interrupt. */
void pic_unmask_irq(uint8_t irq)
{
    (void)irq;
}

/** Nothing: no row installs the handler. */
void set_isr_handler(uint8_t vector, isr_handler_t handler, const char *name)
{
    (void)vector;
    (void)handler;
    (void)name;
}

/** Put the ring's head and tail at `at`, the look ahead's cursor at
 * `looked`, and the `n` bytes of `s` in the ring after the head. */
static void seed(uint32_t at, uint32_t looked, const char *s, int n)
{
    int i;

    _serial_port_exists = 1;
    _rx_head = _rx_tail = at;
    _rx_looked = looked;
    _rx_scanned = at;
    for (i = 0; i < n; i++) {
        _rx_ring[_rx_tail & (SERIAL_RX_SIZE - 1)] = (unsigned char)s[i];
        _rx_tail++;
    }
}

/**
 * A cursor far behind the head, as the head's moves left it before
 * `_rx_advance` kept it up: one fresh byte past a head at 0xc0000000 with
 * the cursor at 0, which a signed distance reads as a gigabyte ahead. The
 * look ahead gives the fresh byte alone, and the head's byte counts as not
 * yet seen.
 */
static void stale_cursor(void)
{
    unsigned char buf[64];

    seed(0xc0000000u, 0, "q", 1);
    CHECK(!serial_rx_looked(), "a cursor the head passed does not mark the head's byte seen");
    CHECK(serial_rx_lookahead(buf, (int)sizeof(buf)) == 1 && buf[0] == 'q',
          "the look ahead gives the fresh byte alone, not the ring's stale positions");
    CHECK(serial_rx_looked() && serial_rx_lookahead(buf, (int)sizeof(buf)) == 0,
          "the byte it gave counts as seen, and nothing is given twice");
}

/**
 * Counters that cross the 32-bit wrap while the ring drains: the look
 * ahead's cursor and the take each cross it, a cursor the takes pass is
 * brought up to the head, and the look ahead then gives the byte after.
 */
static void wrap(void)
{
    unsigned char buf[8];

    seed(0xfffffffeu, 0xfffffffeu, "abcd", 4);
    CHECK(_rx_tail == 2u, "the seeded tail has crossed the wrap");
    CHECK(serial_rx_lookahead(buf, 2) == 2 && buf[0] == 'a' && buf[1] == 'b',
          "the look ahead gives the first two bytes");
    CHECK(serial_rx_take() == 'a' && serial_rx_looked(), "the second byte stays seen after a take");
    CHECK(serial_rx_take() == 'b' && !serial_rx_looked(),
          "the third byte is not seen once the take crosses the wrap");
    CHECK(serial_rx_take() == 'c' && _rx_looked == _rx_head,
          "a take past the cursor brings it up to the head");
    CHECK(serial_rx_lookahead(buf, (int)sizeof(buf)) == 1 && buf[0] == 'd',
          "the look ahead then gives the last byte alone");
}

/**
 * The transmit reads the line status once for each load of the FIFO: 40
 * bytes to a 16550A take three reads, one before each load of 16, and to a
 * UART without FIFOs one read a byte, as before. A transmitter found busy
 * at the start of a load is read until it empties, and the load after is
 * whole.
 */
static void transmit_load(void)
{
    int i;

    _serial_port_exists = 0;
    uart_fifo = 1;
    serial_init();
    CHECK(_serial_port_exists && _tx_load == SERIAL_TX_FIFO,
          "a UART whose FIFOs are on takes 16 bytes a load");
    uart_status_reads = uart_bytes = 0;
    for (i = 0; i < 40; i++) {
        serial_out('x');
    }
    CHECK(uart_bytes == 40 && uart_status_reads == 3,
          "40 bytes to a 16550A read the line status three times");

    _tx_room = 0;
    uart_busy_reads = 2;
    uart_status_reads = uart_bytes = 0;
    for (i = 0; i < 16; i++) {
        serial_out('y');
    }
    CHECK(uart_bytes == 16 && uart_status_reads == 3,
          "a busy transmitter is read until it empties, and then takes a whole load");

    _serial_port_exists = 0;
    _tx_load = 1;
    _tx_room = 0;
    uart_fifo = 0;
    serial_init();
    CHECK(_serial_port_exists && _tx_load == 1, "a UART without FIFOs takes one byte a load");
    uart_status_reads = uart_bytes = 0;
    for (i = 0; i < 5; i++) {
        serial_out('z');
    }
    CHECK(uart_bytes == 5 && uart_status_reads == 5,
          "each byte to a UART without FIFOs reads the line status first");
}

/**
 * Run the rows of a look ahead's cursor far behind the head (`stale_cursor`)
 * and across the counters' 32-bit wrap (`wrap`), and of the transmit's loads
 * (`transmit_load`), and print the count of failed checks. Exits 0 when
 * every check passes, and 1 otherwise.
 */
int main(void)
{
    stale_cursor();
    wrap();
    transmit_load();
    printf("serial-test: result: %d failures\n", failures);
    return failures ? 1 : 0;
}
