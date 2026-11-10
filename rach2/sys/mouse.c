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
#include "timer.h"

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
    if (!(caps & MOUSE_NO_MIDDLE)) {
        evdev_set_capability(dev, EV_KEY, BTN_MIDDLE);
    }
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
    if (!evdev_grabbed(dev)) {
        mouse_push_packet((uint8_t)(buttons & 0x07u), dx, -dy);
    }
}

/* A touch shorter than this is a tap, a click of the left button, as
 * mousedev's `tap_time` has it. */
#define TAP_MS 200

/* The fractions of a count, a movement's denominator, as mousedev keeps. */
#define FRACTION 128

/*
 * The touchpad attached with `mouse_attach_touchpad`, and the state its
 * handler keeps between events, as mousedev's: the positions of the last
 * four packets, a ring indexed by the packet count; the movement and
 * buttons of the packet under way, the buttons a bit each as `mouse_report`
 * numbers them; and the fractions of a count carried to the next packet.
 */
static struct {
    struct evdev *dev;
    int touching;
    uint32_t touch_tick; /* the tick at which the touch began */
    unsigned int count;  /* the packets since the touch began */
    int old_x[4];
    int old_y[4];
    int dx;
    int dy;
    int frac_dx;
    int frac_dy;
    unsigned int buttons;
} pad;

#define PAD_X(i) pad.old_x[(pad.count - (i)) & 3u]
#define PAD_Y(i) pad.old_y[(pad.count - (i)) & 3u]

/** The movement `value` - `old` of the touchpad in counts, scaled by the
 * pad's width `size`, with the fraction carried in `*frac`. */
static int pad_move(int value, int old, int size, int *frac)
{
    int tmp = (value - old) * 256 * FRACTION / size + *frac;
    int move = tmp / FRACTION;

    *frac = tmp - move * FRACTION;
    return move;
}

/** The width of the touchpad's X axis, or 512 when it has none, as
 * mousedev uses it for both axes. */
static int pad_size(struct evdev *dev)
{
    struct input_absinfo a;
    int size = 0;

    if (evdev_get_abs(dev, ABS_X, &a) == 0) {
        size = a.maximum - a.minimum;
    }
    return size ? size : 256 * 2;
}

/** A finger's touch, `down` nonzero, or its lift, which is a tap when the
 * touch lasted less than TAP_MS, as mousedev's `mousedev_touchpad_touch`. */
static void pad_touch(int down)
{
    if (down) {
        if (!pad.touching) {
            pad.touching = 1;
            pad.touch_tick = (uint32_t)kernel_ticks;
        }
        return;
    }
    if (pad.touching && (uint32_t)kernel_ticks - pad.touch_tick < MS_TO_TICKS(TAP_MS)) {
        mouse_push_packet((uint8_t)(pad.buttons | MOUSE_LEFT), 0, 0);
    }
    pad.touching = 0;
    pad.count = 0;
    pad.frac_dx = 0;
    pad.frac_dy = 0;
}

/** The button bit, as `mouse_report` numbers them, of the key `code`, as
 * mousedev's `mousedev_key_event` names them, or 0 for a key it does not
 * name. */
static unsigned int button_bit(uint16_t code)
{
    switch (code) {
    case BTN_0:
    case BTN_LEFT:
        return MOUSE_LEFT;
    case BTN_1:
    case BTN_RIGHT:
        return MOUSE_RIGHT;
    case BTN_FORWARD:
    case BTN_MIDDLE:
        return MOUSE_MIDDLE;
    case BTN_BACK:
    case BTN_SIDE:
        return MOUSE_BUTTON_SIDE;
    case BTN_EXTRA:
        return MOUSE_BUTTON_EXTRA;
    default:
        return 0;
    }
}

/** Hold or let go of the button `code` names; a key that names none is
 * ignored. */
static void pad_key(uint16_t code, int down)
{
    unsigned int bit = button_bit(code);

    pad.buttons = down ? pad.buttons | bit : pad.buttons & ~bit;
}

/**
 * The handler of the touchpad's events, mousedev's `mousedev_event` for a
 * touchpad: ABS_X and ABS_Y move it, BTN_TOUCH starts and ends a touch,
 * the buttons are held and let go, and SYN_REPORT pushes the packet. The
 * input core drops a value that has not changed, so each SYN_REPORT of a
 * touch carries the last position forward into the next packet's place.
 */
static void pad_event(struct evdev *dev, uint16_t type, uint16_t code, int32_t value)
{
    if (dev != pad.dev) {
        return;
    }
    switch (type) {
    case EV_ABS:
        if (code == ABS_X) {
            PAD_X(0) = value;
            if (pad.touching && pad.count >= 2) {
                pad.dx = pad_move(value, PAD_X(2), pad_size(dev), &pad.frac_dx);
            }
        } else if (code == ABS_Y) {
            PAD_Y(0) = value;
            if (pad.touching && pad.count >= 2) {
                pad.dy = pad_move(PAD_Y(2), value, pad_size(dev), &pad.frac_dy);
            }
        }
        break;
    case EV_KEY:
        if (value == 2) {
            break;
        }
        if (code == BTN_TOUCH) {
            pad_touch(value);
        } else {
            pad_key(code, value);
        }
        break;
    case EV_SYN:
        if (code != SYN_REPORT) {
            break;
        }
        if (pad.touching) {
            pad.count++;
            PAD_X(0) = PAD_X(1);
            PAD_Y(0) = PAD_Y(1);
        }
        mouse_push_packet((uint8_t)pad.buttons, pad.dx, pad.dy);
        pad.dx = 0;
        pad.dy = 0;
        break;
    default:
        break;
    }
}

/* The packet under way of the events written to a mouse's event device, as
 * mousedev accumulates a relative device's: the movement, up positive, and
 * the buttons held, and those of the last packet pushed. */
static struct {
    int dx;
    int dy;
    unsigned int buttons;
    unsigned int pushed;
} written;

void mouse_written(uint16_t type, uint16_t code, int32_t value)
{
    unsigned int bit;

    switch (type) {
    case EV_REL:
        if (code == REL_X) {
            written.dx += value;
        } else if (code == REL_Y) {
            written.dy -= value;
        }
        break;
    case EV_KEY:
        bit = button_bit(code);
        if (value != 2) {
            written.buttons = value ? written.buttons | bit : written.buttons & ~bit;
        }
        break;
    case EV_SYN:
        if (code != SYN_REPORT ||
            (written.dx == 0 && written.dy == 0 && written.buttons == written.pushed)) {
            break;
        }
        mouse_push_packet((uint8_t)written.buttons, written.dx, written.dy);
        written.pushed = written.buttons;
        written.dx = 0;
        written.dy = 0;
        break;
    default:
        break;
    }
}

void mouse_attach_touchpad(struct evdev *dev)
{
    if (!dev) {
        return;
    }
    k_memset(&pad, 0, sizeof(pad));
    pad.dev = dev;
    evdev_set_handler(dev, pad_event);
}
