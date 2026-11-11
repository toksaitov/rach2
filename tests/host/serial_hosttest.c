/**
 * Host test of the console serial port's receive ring, arch/x86/serial.c:
 * the look ahead's cursor at counters near the 32-bit wrap and at a cursor
 * the head has passed by far.
 *
 * host-deps:
 *
 * The rows set the ring's counters and bytes directly, as the port's
 * interrupt leaves them, and read them back through serial.h's calls; no
 * UART is modelled, since the rows reach none of its registers.
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

/** Nothing: no row writes a port. */
void outb(uint16_t port, uint8_t val)
{
    (void)port;
    (void)val;
}

/** 0, a line status with no byte ready: no row reads the UART. */
uint8_t inb(uint16_t port)
{
    (void)port;
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
 * Run the rows of a look ahead's cursor far behind the head (`stale_cursor`)
 * and across the counters' 32-bit wrap (`wrap`), and print the count of
 * failed checks. Exits 0 when every check passes, and 1 otherwise.
 */
int main(void)
{
    stale_cursor();
    wrap();
    printf("serial-test: result: %d failures\n", failures);
    return failures ? 1 : 0;
}
