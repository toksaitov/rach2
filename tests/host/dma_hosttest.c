/* ISA DMA channel masks against a model of the two single-mask ports. */

#include "audio/dma.c"

extern int printf(const char *format, ...);
static unsigned writes;
static uint16_t last_port;
static uint8_t last_value;

/** Record the port and mask byte written by the driver. */
void outb(uint16_t port, uint8_t value)
{
    writes++;
    last_port = port;
    last_value = value;
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
    printf("dma-test: result: %s\n", failures ? "fail" : "pass");
    return failures ? 1 : 0;
}
