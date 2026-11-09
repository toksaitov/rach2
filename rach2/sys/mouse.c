/*
 * mouse.c: each mouse's event device, and the one queue of mouse packets a
 * program reads through the mouse device, filled by the platform's mouse
 * and a USB one alike, so that the reader cannot tell which moved.
 */

#include "mouse.h"
#include "platform.h"

#include "display/blank.h"
#include "evdev.h"
#include "lib/kstring.h"

/* The queue: the mouse drivers its one producer, at `mouse_tail`, from
 * interrupt context, and `mouse_read_packets` its one consumer, at
 * `mouse_head`; a slot stays empty to tell a full queue from an empty one. */
#define MOUSE_BUF_SIZE 64
static struct mouse_packet mouse_buf[MOUSE_BUF_SIZE];
static volatile int mouse_head = 0;
static volatile int mouse_tail = 0;

void mouse_push_packet(uint8_t buttons, int dx, int dy)
{
    int next = (mouse_tail + 1) % MOUSE_BUF_SIZE;

    console_poke();
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

struct evdev *mouse_register(const char *name, const char *phys, uint16_t bustype, uint16_t vendor,
                             uint16_t product, unsigned int caps)
{
    uint8_t keys[EVDEV_KEY_BYTES];
    struct evdev *dev;

    k_memset(keys, 0, sizeof(keys));
    dev = evdev_register(name, bustype, vendor, product, keys);
    evdev_describe(dev, phys, 0);
    evdev_set_capability(dev, EV_KEY, BTN_LEFT);
    evdev_set_capability(dev, EV_KEY, BTN_RIGHT);
    evdev_set_capability(dev, EV_KEY, BTN_MIDDLE);
    evdev_set_capability(dev, EV_REL, REL_X);
    evdev_set_capability(dev, EV_REL, REL_Y);
    if (caps & MOUSE_SIDE) {
        evdev_set_capability(dev, EV_KEY, BTN_SIDE);
        evdev_set_capability(dev, EV_KEY, BTN_EXTRA);
    }
    if (caps & MOUSE_WHEEL) {
        evdev_set_capability(dev, EV_REL, REL_WHEEL);
        if (caps & MOUSE_HI_RES) {
            evdev_set_capability(dev, EV_REL, REL_WHEEL_HI_RES);
        }
    }
    if (caps & MOUSE_HWHEEL) {
        evdev_set_capability(dev, EV_REL, REL_HWHEEL);
        if (caps & MOUSE_HI_RES) {
            evdev_set_capability(dev, EV_REL, REL_HWHEEL_HI_RES);
        }
    }
    if (caps & MOUSE_POINTER) {
        evdev_set_prop(dev, INPUT_PROP_POINTER);
    }
    return dev;
}

void mouse_report(struct evdev *dev, unsigned int buttons, int dx, int dy, int wheel, int hwheel)
{
    static const uint16_t codes[5] = {BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA};
    int i;

    for (i = 0; i < 5; i++) {
        evdev_report(dev, EV_KEY, codes[i], (buttons >> i) & 1u);
    }
    evdev_report(dev, EV_REL, REL_X, dx);
    evdev_report(dev, EV_REL, REL_Y, dy);
    evdev_report(dev, EV_REL, REL_WHEEL, wheel);
    evdev_report(dev, EV_REL, REL_WHEEL_HI_RES, wheel * 120);
    evdev_report(dev, EV_REL, REL_HWHEEL, hwheel);
    evdev_report(dev, EV_REL, REL_HWHEEL_HI_RES, hwheel * 120);
    evdev_report(dev, EV_SYN, SYN_REPORT, 0);
    mouse_push_packet((uint8_t)(buttons & 0x07u), dx, -dy);
}
