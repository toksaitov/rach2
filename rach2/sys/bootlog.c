/*
 * bootlog.c: the machine's own copy of what it printed.
 *
 * A plain ring with a total count. Nothing here allocates, blocks or takes a
 * lock, because it is called from inside `serial_putc` and therefore from
 * interrupt handlers and from the panic path; a character is stored and the
 * function returns.
 */

#include "bootlog.h"

static char log_ring[BOOTLOG_SIZE];

/* Where the next character goes, and how many have ever been written. The
   count is what makes the reader's offsets stable: once the ring has wrapped,
   the oldest byte still held is at `written - BOOTLOG_SIZE`. */
static uint32_t log_head;
static uint32_t log_written;

void bootlog_putc(char c)
{
    log_ring[log_head] = c;
    log_head++;
    if (log_head == BOOTLOG_SIZE) {
        log_head = 0;
    }
    log_written++;
}

uint32_t bootlog_len(void)
{
    return (log_written < BOOTLOG_SIZE) ? log_written : BOOTLOG_SIZE;
}

uint32_t bootlog_read(char *buf, uint32_t cap, uint32_t offset)
{
    uint32_t len = bootlog_len();
    uint32_t start;
    uint32_t n = 0;

    if (!buf || cap == 0 || offset >= len) {
        return 0;
    }
    /* Where the oldest byte held sits in the ring. Before the first wrap that
     * is the beginning; after it, the head, because the head is both the next
     * write and the oldest survivor. */
    start = (log_written < BOOTLOG_SIZE) ? 0 : log_head;

    while (n < cap && offset + n < len) {
        uint32_t i = start + offset + n;

        if (i >= BOOTLOG_SIZE) {
            i -= BOOTLOG_SIZE;
        }
        buf[n] = log_ring[i];
        n++;
    }
    return n;
}
