/**
 * Host test of the console serial port, arch/x86/serial.c: the receive
 * ring's look ahead cursor at counters near the 32-bit wrap and at a cursor
 * the head has passed by far, the transmit's reads of the line status for
 * each load of the FIFO, and a report that interrupts a writer.
 *
 * host-deps:
 *
 * The ring's rows set its counters and bytes directly, as the port's
 * interrupt leaves them, and read them back through serial.h's calls. The
 * transmit's rows run against a model of the UART's registers that counts
 * the reads of the line status and the bytes written, keeps the FIFO's
 * fill and counts the bytes written to a full one, and raises an interrupt,
 * whose handler writes a report, once interrupts are on after a chosen read
 * of the line status.
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
 * still find the transmitter busy, the counts of status reads and of bytes
 * written to the data port, the bytes in the transmit FIFO and those
 * written while it was full. */
static int uart_fifo;
static int uart_busy_reads;
static int uart_status_reads;
static int uart_bytes;
static int uart_fill;
static int uart_overruns;
static uint8_t uart_scratch;

/* The model processor: its interrupt flag, the status read after which
 * the interrupt is raised (0 for none), whether it waits for the flag, and
 * the bytes its handler's report writes. */
static int cpu_irq_on = 1;
static int irq_at_read;
static int irq_raised;
static int irq_report;

/** Run the raised interrupt's handler, which writes its report to the
 * console as a lock checker's report does, if interrupts are on. */
static void irq_deliver(void)
{
    int i;

    if (!irq_raised || !cpu_irq_on) {
        return;
    }
    irq_raised = 0;
    for (i = 0; i < irq_report; i++) {
        serial_out('r');
    }
}

/** Count a byte written to the data port, and one written while the
 * FIFO, or a UART's one holding register, was full, and keep one written
 * to the scratch register; ignore the other registers. */
void outb(uint16_t port, uint8_t val)
{
    if (port == SERIAL_COM1 + SERIAL_DATA) {
        uart_bytes++;
        if (uart_fill >= (uart_fifo ? SERIAL_TX_FIFO : 1)) {
            uart_overruns++;
        } else {
            uart_fill++;
        }
    }
    if (port == SERIAL_COM1 + SERIAL_SCRATCH) {
        uart_scratch = val;
    }
}

/** The model's registers: the interrupt identification shows the FIFOs
 * when they are on, the scratch register reads back what the probe wrote,
 * and the line status shows the transmitter empty, its FIFO drained, and no
 * byte ready once `uart_busy_reads` reads have found it busy. The
 * `irq_at_read`th status read raises the interrupt, which runs at once if
 * interrupts are on, and otherwise when they are turned on. */
uint8_t inb(uint16_t port)
{
    uint8_t lsr = SERIAL_LSR_THRE;

    if (port == SERIAL_COM1 + SERIAL_INTR_ID) {
        return uart_fifo ? 0xC1 : 0x01;
    }
    if (port == SERIAL_COM1 + SERIAL_SCRATCH) {
        return uart_scratch;
    }
    if (port != SERIAL_COM1 + SERIAL_LINE_STATUS) {
        return 0;
    }
    uart_status_reads++;
    if (uart_busy_reads > 0) {
        uart_busy_reads--;
        lsr = 0;
    } else {
        uart_fill = 0;
    }
    if (irq_at_read != 0 && uart_status_reads == irq_at_read) {
        irq_raised = 1;
        irq_deliver();
    }
    return lsr;
}

/** Turn the model processor's interrupts off and return whether they were
 * on. */
uint32_t kirq_save(void)
{
    uint32_t was = (uint32_t)cpu_irq_on;

    cpu_irq_on = 0;
    return was;
}

/** Put the interrupt flag back, and run an interrupt raised meanwhile if
 * that turns interrupts on. */
void kirq_restore(uint32_t flags)
{
    cpu_irq_on = (int)flags;
    irq_deliver();
}

/** The host's single thread: the exchange needs no atomicity. */
unsigned int arch_atomic_cmpxchg(volatile unsigned int *p, unsigned int old, unsigned int value)
{
    unsigned int was = *p;

    if (was == old) {
        *p = value;
    }
    return was;
}

/** As `arch_atomic_cmpxchg`, unconditionally. */
unsigned int arch_atomic_xchg(volatile unsigned int *p, unsigned int value)
{
    unsigned int was = *p;

    *p = value;
    return was;
}

/** Nothing: no row finds the lock held by another processor. */
void arch_relax(void)
{}

/** 0: no row panics. */
int k_panicking(void)
{
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
 * whole, after one more read under the transmitter's lock, since another
 * writer may have filled the FIFO between the wait and the lock.
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
    CHECK(uart_bytes == 16 && uart_status_reads == 4,
          "a busy transmitter is read until it empties, read again under the lock, and then "
          "takes a whole load");

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
 * A report that an interrupt writes in the middle of a console write, as a
 * lock checker's report does: the interrupt is raised just after the
 * writer's status read finds the FIFO empty, before the writer counts the
 * room that read gave. The report runs once the writer gives back the
 * transmitter's lock, and the two count one FIFO's room between them, so
 * that no byte reaches a full FIFO. Without the lock, the report fills ten
 * places and the writer then counts all 16 free, and ten of its bytes
 * overrun the FIFO.
 */
static void transmit_overlap(void)
{
    int i;

    _serial_port_exists = 0;
    _tx_room = 0;
    uart_fifo = 1;
    serial_init();
    uart_status_reads = uart_bytes = uart_fill = uart_overruns = 0;
    irq_report = 10;
    irq_at_read = 1;
    for (i = 0; i < 16; i++) {
        serial_out('w');
    }
    irq_at_read = 0;
    CHECK(!irq_raised && uart_bytes == 26, "the report and the writer each send every byte");
    CHECK(uart_overruns == 0, "no byte of the writer's or the report's reaches a full FIFO");
    CHECK(_tx_owner == 0 && cpu_irq_on, "the lock is given back and interrupts are on again");
}

/**
 * Run the rows of a look ahead's cursor far behind the head (`stale_cursor`)
 * and across the counters' 32-bit wrap (`wrap`), of the transmit's loads
 * (`transmit_load`) and of a report between a writer's status read and its
 * count (`transmit_overlap`), and print the count of failed checks. Exits 0
 * when every check passes, and 1 otherwise.
 */
int main(void)
{
    stale_cursor();
    wrap();
    transmit_load();
    transmit_overlap();
    printf("serial-test: result: %d failures\n", failures);
    return failures ? 1 : 0;
}
