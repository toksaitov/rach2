#ifndef _MOUSE_H_
#define _MOUSE_H_

#include "lib/kint.h"
#include "lib/kdef.h"

/*
 * The mouse queue: movement packets from the platform's mouse and from a USB
 * one, in the one queue a program reads through the mouse device.
 */

/*
 * One movement packet, as exposed to userspace.
 */
struct mouse_packet {
    uint8_t buttons; /* bit 0 left, bit 1 right, bit 2 middle */
    int8_t dx;       /* X movement delta */
    int8_t dy;       /* Y movement delta */
};

/**
 * Put one packet in the queue, from whichever mouse produced it.
 *
 * The PS/2 driver calls this when it has assembled three bytes; the USB class
 * driver calls it with a report it decoded. One queue, because a program
 * reading `/dev/mouse` is reading movement rather than a particular device.
 */
void mouse_push_packet(uint8_t buttons, int dx, int dy);

/** Drain up to max_count packets into buf; returns the count copied. */
int mouse_read_packets(struct mouse_packet *buf, int max_count);

/**
 * Discard any buffered packets, and have the platform's mouse start its next
 * packet afresh.
 */
void mouse_clear_packets(void);

#endif /* _MOUSE_H_ */
