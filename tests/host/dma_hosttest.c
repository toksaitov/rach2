/* ISA DMA channel masks and count readings against a model of the ports. */

#include "audio/dma.c"

extern int printf(const char *format, ...);
static unsigned writes;
static uint16_t last_port;
static uint8_t last_value;
static uint16_t count;   /* the model's count register */
static int high_byte;    /* the flip-flop: the next read is the high byte */
static int reads;        /* bytes read from the count port */
static int borrow_after; /* the read after which the count falls; -1 never */
static uint16_t count_port_seen;

/** Record the port and byte written by the driver; reset the flip-flop. */
void outb(uint16_t port, uint8_t value)
{
    writes++;
    last_port = port;
    last_value = value;
    if (port == 0x0C || port == 0xD8)
        high_byte = 0;
}

/** Read the count register a byte at a time, counting it down once. */
uint8_t inb(uint16_t port)
{
    uint8_t byte = high_byte ? (uint8_t)(count >> 8) : (uint8_t)count;

    count_port_seen = port;
    high_byte = !high_byte;
    if (reads++ == borrow_after)
        count--;
    return byte;
}

/** Read `channel` with the count at `start`, falling after read `after`. */
static int read_count(unsigned channel, uint16_t start, int after)
{
    count = start;
    high_byte = 1;
    reads = 0;
    borrow_after = after;
    writes = 0;
    count_port_seen = 0;
    return dma_remaining((uint8_t)channel);
}

/** Ignore diagnostics; these rows stop channels without starting transfers. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

/** Check every channel argument: 0 for correct mask writes, 1 otherwise. */
int main(void)
{
    unsigned channel;
    int failures = 0;

    for (channel = 0; channel < 256; channel++) {
        writes = 0;
        dma_stop((uint8_t)channel);
        if (channel == 4 || channel > 7) {
            if (writes != 0) {
                printf("dma-test: fail: cascade and invalid channels do no I/O\n");
                failures++;
            }
        } else if (writes != 1 || last_port != (channel < 4 ? 0x0A : 0xD4) ||
                   last_value != ((channel & 3) | 4)) {
            printf("dma-test: fail: channel %u mask port and value\n", channel);
            failures++;
        }
    }
    for (channel = 0; channel < 256; channel++) {
        static const uint16_t ports[] = {0x01, 0x03, 0x05, 0x07, 0, 0xC6, 0xCA, 0xCE};
        int got;

        if (channel == 4 || channel > 7) {
            if (read_count(channel, 0x1234, -1) != -1 || writes != 0 || reads != 0) {
                printf("dma-test: fail: cascade and invalid channels read no count\n");
                failures++;
            }
            continue;
        }
        got = read_count(channel, 0x1234, -1);
        if (got != 0x1235 || count_port_seen != ports[channel] ||
            last_port != (channel < 4 ? 0x0C : 0xD8)) {
            printf("dma-test: fail: channel %u count is the register plus one\n", channel);
            failures++;
        }
        got = read_count(channel, 0x0300, 0);
        if (got != 0x0300) {
            printf("dma-test: fail: channel %u count read across a borrow in the first "
                   "reading is %#x\n",
                   channel, (unsigned)got);
            failures++;
        }
        got = read_count(channel, 0x0300, 2);
        if (got != 0x0300) {
            printf("dma-test: fail: channel %u count read across a borrow in the second "
                   "reading is %#x\n",
                   channel, (unsigned)got);
            failures++;
        }
    }
    printf("dma-test: result: %s\n", failures ? "fail" : "pass");
    return failures ? 1 : 0;
}
