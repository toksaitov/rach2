/*
 * mouse.c: the one queue of mouse packets a program reads through the mouse
 * device, filled by the platform's mouse and a USB one alike, so that the
 * reader cannot tell which moved.
 */

#include "mouse.h"
#include "platform.h"

/* Mouse packet ring buffer */
#define MOUSE_BUF_SIZE 64
static struct mouse_packet mouse_buf[MOUSE_BUF_SIZE];
static volatile int mouse_head = 0;
static volatile int mouse_tail = 0;

/**
 * Append a completed packet to the ring buffer, dropping it when full.
 *
 * Runs in IRQ context, from the driver of whichever mouse moved (the
 * producer); the consumer is mouse_read_packets.
 * Deltas are clamped to the int8_t range the packet struct has.
 */
void mouse_push_packet(uint8_t buttons, int dx, int dy)
{
    int next = (mouse_tail + 1) % MOUSE_BUF_SIZE;
    if (next != mouse_head) {
        if (dx > 127)
            dx = 127;
        if (dx < -128)
            dx = -128;
        if (dy > 127)
            dy = 127;
        if (dy < -128)
            dy = -128;

        mouse_buf[mouse_tail].buttons = buttons & 0x07; /* button bits 0-2 */
        mouse_buf[mouse_tail].dx = (int8_t)dx;
        mouse_buf[mouse_tail].dy = (int8_t)dy;
        mouse_tail = next;
    }
}

/**
 * Drain up to max_count packets from the ring buffer into buf.
 *
 * The consumer side of the buffer the mouse drivers fill. It never
 * blocks: it copies whatever packets are present and returns the count,
 * which may be zero.
 *
 * @return the number of packets copied.
 */
int mouse_read_packets(struct mouse_packet *buf, int max_count)
{
    int count = 0;

    while (count < max_count && mouse_head != mouse_tail) {
        buf[count++] = mouse_buf[mouse_head];
        mouse_head = (mouse_head + 1) % MOUSE_BUF_SIZE;
    }

    return count;
}

void mouse_clear_packets(void)
{
    mouse_head = 0;
    mouse_tail = 0;
    platform_mouse_resync();
}
