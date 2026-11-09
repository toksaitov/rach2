/*
 * mouse.h: the mice, each an event device of its own, and the one queue of
 * movement packets from all of them that a program reads through the mouse
 * device.
 */
#ifndef _MOUSE_H_
#define _MOUSE_H_

#include "lib/kint.h"
#include "lib/kdef.h"

struct evdev;

/*
 * One movement packet, as a read of `/mouse` returns it.
 */
struct mouse_packet {
    uint8_t buttons; /* bit 0 left, bit 1 right, bit 2 middle */
    int8_t dx;       /* X movement delta, positive to the right */
    int8_t dy;       /* Y movement delta, positive away from the user */
};

/**
 * Put one packet in the queue, from whichever mouse produced it, in
 * interrupt context; a full queue of 63 packets drops it. Each packet puts
 * off the console's blanking (`console_poke`).
 *
 * The PS/2 driver calls this when it has assembled three bytes; the USB class
 * driver calls it with a report it decoded. One queue, because a program
 * reading `/mouse` is reading movement rather than a particular device.
 * The deltas are clamped to -128 to 127 and the buttons to their low three
 * bits. A positive `dy` is a movement away from the user, as the PS/2
 * report's Y has it; the USB class driver negates its report's Y, which is
 * positive toward the user, so the two mice give the same `dy` for the same
 * movement.
 */
void mouse_push_packet(uint8_t buttons, int dx, int dy);

/* What a mouse has beyond X, Y and its left, right and middle buttons, for
 * `mouse_register`. */
#define MOUSE_WHEEL   0x01 /* a wheel, REL_WHEEL */
#define MOUSE_HWHEEL  0x02 /* a horizontal wheel, REL_HWHEEL */
#define MOUSE_HI_RES  0x04 /* each wheel's turn in 120ths as well, REL_*WHEEL_HI_RES */
#define MOUSE_SIDE    0x08 /* the side and extra buttons, BTN_SIDE and BTN_EXTRA */
#define MOUSE_POINTER 0x10 /* INPUT_PROP_POINTER, which Linux's PS/2 driver sets */

/* The buttons of `mouse_report`, a bit each. */
#define MOUSE_LEFT         0x01
#define MOUSE_RIGHT        0x02
#define MOUSE_MIDDLE       0x04
#define MOUSE_BUTTON_SIDE  0x08
#define MOUSE_BUTTON_EXTRA 0x10

/**
 * Register a mouse as an event device: `name`, `phys`, `bustype`, `vendor`
 * and `product` as `evdev_register` and `evdev_describe` take them, and
 * `caps` MOUSE_*. The device has REL_X, REL_Y, BTN_LEFT, BTN_RIGHT and
 * BTN_MIDDLE, and what `caps` adds. Any context. Returns the device, or
 * NULL when every node is taken, which `mouse_report` takes as well.
 */
struct evdev *mouse_register(const char *name, const char *phys, uint16_t bustype, uint16_t vendor,
                             uint16_t product, unsigned int caps);

/**
 * Report one packet of the mouse `dev`, in interrupt context or from the
 * USB stack's step: `buttons` MOUSE_* held, the motion `dx` to the right
 * and `dy` toward the user, as Linux's REL_X and REL_Y have them, and the
 * wheels' turns in detents, `wheel` up and `hwheel` to the right positive.
 * The event device gets the buttons that changed, the motion that is not 0
 * and SYN_REPORT, with each turn in 120ths too on a MOUSE_HI_RES mouse; the
 * mouse queue gets the packet as `mouse_push_packet` takes it, `dy` turned
 * the other way up, without the wheels.
 */
void mouse_report(struct evdev *dev, unsigned int buttons, int dx, int dy, int wheel, int hwheel);

/** Drain up to `max_count` packets into `buf` without waiting; returns the
 * count copied, which may be zero. */
int mouse_read_packets(struct mouse_packet *buf, int max_count);

/**
 * Discard any buffered packets, and have the platform's mouse start its next
 * packet afresh, when a process opens the mouse.
 */
void mouse_clear_packets(void);

#endif /* _MOUSE_H_ */
