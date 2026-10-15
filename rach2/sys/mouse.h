/*
 * mouse.h: the mouse queue, movement packets from the platform's mouse and
 * from a USB one, in the one queue a program reads through the mouse device.
 */
#ifndef _MOUSE_H_
#define _MOUSE_H_

#include "lib/kint.h"
#include "lib/kdef.h"

/*
 * One movement packet, as a read of `/dev/mouse` returns it.
 */
struct mouse_packet {
    uint8_t buttons; /* bit 0 left, bit 1 right, bit 2 middle */
    int8_t dx;       /* X movement delta */
    int8_t dy;       /* Y movement delta, its sign as the driver gave it */
};

/**
 * Put one packet in the queue, from whichever mouse produced it, in
 * interrupt context; a full queue of 63 packets drops it.
 *
 * The PS/2 driver calls this when it has assembled three bytes; the USB class
 * driver calls it with a report it decoded. One queue, because a program
 * reading `/dev/mouse` is reading movement rather than a particular device.
 * The deltas are clamped to -128 to 127 and the buttons to their low three
 * bits. Each driver negates its device's Y: the PS/2 report's is positive
 * upward and the USB report's positive downward, so the two give `dy` the
 * opposite sign for the same movement.
 */
void mouse_push_packet(uint8_t buttons, int dx, int dy);

/** Drain up to `max_count` packets into `buf` without waiting; returns the
 * count copied, which may be zero. */
int mouse_read_packets(struct mouse_packet *buf, int max_count);

/**
 * Discard any buffered packets, and have the platform's mouse start its next
 * packet afresh, when a process opens the mouse.
 */
void mouse_clear_packets(void);

#endif /* _MOUSE_H_ */
