/*
 * hid_parse_hosttest.c: the HID report descriptor parser, usb/hid_parse.c,
 * and the mapping of its usages to event devices, usb/hid_input.c, fed the
 * descriptors and reports of QEMU's USB tablet, keyboard and mouse and of
 * made-up devices, with the event devices modelled.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * The capabilities and events each row expects are those Linux 6.12's
 * hid-core.c, hid-input.c and hid-generic.c give the same descriptor, as
 * evtest prints them on Debian: QEMU's tablet is "QEMU QEMU USB Tablet",
 * bus 3, 0627:0001, version 1, with BTN_LEFT to BTN_EXTRA, REL_WHEEL and
 * REL_WHEEL_HI_RES, ABS_X and ABS_Y over 0 to 32767, and MSC_SCAN. The model
 * of an event device drops what Linux's input core drops before a reader
 * sees it: a key already in the state reported, a relative motion of 0, an
 * absolute value that did not change, MSC_SCAN on a device without it, and
 * a SYN_REPORT that ends an empty packet.
 */

#include "../../rach2/sys/usb/hid_input.h"

extern int printf(const char *fmt, ...);
extern int snprintf(char *s, unsigned long n, const char *fmt, ...);
extern void *calloc(unsigned long n, unsigned long size);
extern void free(void *p);
extern void exit(int status);
extern int strcmp(const char *a, const char *b);
extern void *memset(void *s, int c, unsigned long n);
extern void *memcpy(void *d, const void *s, unsigned long n);

/* ---- the functions outside hid_parse.c and hid_input.c that they call ---- */

/* The absolute axes, Linux's ABS_CNT, which hid_input.c names later. */
#define AXES      64
#define SLOT_AXES (ABS_MT_LAST - ABS_MT_FIRST + 1)

/* One event device as it was registered and described. */
struct evdev {
    char name[HID_NAME_MAX];
    const char *phys;
    uint16_t bus;
    uint16_t vendor;
    uint16_t product;
    uint16_t version;
    unsigned int flags; /* EVDEV_SCAN, EVDEV_KEYBOARD */
    uint8_t keys[EVDEV_KEY_BYTES];
    uint8_t rel[2];
    uint8_t abs[8];
    uint8_t led[2];
    int32_t absmin[AXES];
    int32_t absmax[AXES];
    int32_t fuzz[AXES];
    int32_t flat[AXES];
    int32_t res[AXES];
    int32_t absval[AXES];
    unsigned int props;
    int slots, slot, trkid;
    int emitted_slot;
    int32_t slot_value[EVDEV_MT_SLOTS][SLOT_AXES];
    int key[EVDEV_MT_SLOTS];
    int id[EVDEV_MT_SLOTS];
    uint8_t used[EVDEV_MT_SLOTS];
    uint8_t down[EVDEV_KEY_BYTES];
    int pending;  /* events since the last SYN_REPORT */
    int keyboard; /* attached to the input layer as a keyboard */
    int gone;
    char uniq[64];
    unsigned int lit; /* the LEDs a program lit, for evdev_leds_lit */
    struct evdev_keymap km;
};

#define MAX_DEVS 16
static struct evdev devs[MAX_DEVS];
static int ndevs;

/* The events readers saw, in order, and the mouse queue's packets. */
struct seen {
    struct evdev *dev;
    uint16_t type;
    uint16_t code;
    int32_t value;
};
#define MAX_SEEN 256
static struct seen seen[MAX_SEEN];
static int nseen;
static int mice_found;
static int packets;
static unsigned packet_buttons;
static int packet_dx;
static int packet_dy;

/** Whether bit `n` of `bits` is set. */
static int bit(const uint8_t *bits, unsigned n)
{
    return (bits[n >> 3] >> (n & 7)) & 1;
}

/** Set bit `n` of `bits`. */
static void put(uint8_t *bits, unsigned n)
{
    bits[n >> 3] |= (uint8_t)(1u << (n & 7));
}

/** Reserve a model event device. */
struct evdev *evdev_reserve(void)
{
    struct evdev *d;

    if (ndevs == MAX_DEVS) {
        return NULL;
    }
    d = &devs[ndevs++];
    memset(d, 0, sizeof(*d));
    return d;
}

/** Publish a model event device and add its key set. */
struct evdev *evdev_register_reserved(struct evdev *d, const char *name, uint16_t bustype,
                                      uint16_t vendor, uint16_t product, const uint8_t *keys)
{
    unsigned int i;

    if (!d) {
        d = evdev_reserve();
    }
    if (!d) {
        return NULL;
    }
    snprintf(d->name, sizeof(d->name), "%s", name);
    d->bus = bustype;
    d->vendor = vendor;
    d->product = product;
    for (i = 0; i < EVDEV_KEY_BYTES; i++) {
        d->keys[i] |= keys[i];
    }
    return d;
}

/** Register a model event device with key set `keys`. */
struct evdev *evdev_register(const char *name, uint16_t bustype, uint16_t vendor, uint16_t product,
                             const uint8_t *keys)
{
    return evdev_register_reserved(NULL, name, bustype, vendor, product, keys);
}

/** Keep the phys and flags of `dev`. */
void evdev_describe(struct evdev *dev, const char *phys, unsigned int flags)
{
    dev->phys = phys;
    dev->flags = flags;
}

/** Keep the keymap of `dev`. */
void evdev_set_keymap(struct evdev *dev, const struct evdev_keymap *km)
{
    dev->km = *km;
}

/** Keep the serial of `dev`. */
void evdev_set_uniq(struct evdev *dev, const char *uniq)
{
    snprintf(dev->uniq, sizeof(dev->uniq), "%s", uniq ? uniq : "");
}

/** The LEDs a row lit on `dev`. */
unsigned int evdev_leds_lit(struct evdev *dev)
{
    return dev ? dev->lit : 0;
}

/** Keep the version of `dev`. */
void evdev_set_version(struct evdev *dev, uint16_t version)
{
    dev->version = version;
}

/** Give `dev` the event `type`, `code`. */
void evdev_set_capability(struct evdev *dev, uint16_t type, uint16_t code)
{
    if (type == EV_REL) {
        put(dev->rel, code);
    } else if (type == EV_LED) {
        put(dev->led, code);
    } else if (type == EV_KEY) {
        put(dev->keys, code);
    }
}

/** Give `dev` the axis `code` with its limits. */
void evdev_set_abs(struct evdev *dev, uint16_t code, int32_t minimum, int32_t maximum, int32_t fuzz,
                   int32_t flat)
{
    put(dev->abs, code);
    dev->absmin[code] = minimum;
    dev->absmax[code] = maximum;
    dev->fuzz[code] = fuzz;
    dev->flat[code] = flat;
}

/** Keep the resolution of axis `code` of `dev`. */
void evdev_set_abs_res(struct evdev *dev, uint16_t code, int32_t resolution)
{
    dev->res[code] = resolution;
}

/** Record a property of the model event device. */
void evdev_set_prop(struct evdev *dev, unsigned int prop)
{
    dev->props |= 1u << prop;
}

/** Set up slots on the model device. */
int evdev_init_slots(struct evdev *dev, int slots, unsigned int flags)
{
    int i;

    dev->slots = slots;
    evdev_set_abs(dev, ABS_MT_SLOT, 0, slots - 1, 0, 0);
    evdev_set_abs(dev, ABS_MT_TRACKING_ID, 0, 65535, 0, 0);
    put(dev->keys, BTN_TOUCH);
    if (flags & INPUT_MT_POINTER) {
        evdev_set_prop(dev, INPUT_PROP_POINTER);
    }
    if (flags & INPUT_MT_DIRECT) {
        evdev_set_prop(dev, INPUT_PROP_DIRECT);
    }
    for (i = 0; i < slots; i++) {
        dev->id[i] = -1;
        dev->slot_value[i][ABS_MT_TRACKING_ID - ABS_MT_FIRST] = -1;
    }
    return 0;
}

/** Find an active contact, or reserve the first unused inactive slot. */
int evdev_mt_get_slot_by_key(struct evdev *dev, int32_t key)
{
    int i, unused = -1;

    for (i = 0; i < dev->slots; i++) {
        if (dev->id[i] >= 0 && dev->key[i] == key) {
            return i;
        }
        if (unused < 0 && dev->id[i] < 0 && !dev->used[i]) {
            unused = i;
        }
    }
    if (unused >= 0) {
        dev->key[unused] = key;
    }
    return unused;
}

/** Record a slot's tracking ID. */
int evdev_mt_report_slot_state(struct evdev *dev, int tool, int active)
{
    int *id = &dev->id[dev->slot];

    dev->used[dev->slot] = 1;
    if (active && *id < 0) {
        *id = dev->trkid++;
        evdev_report(dev, EV_ABS, ABS_MT_TRACKING_ID, *id);
    } else if (!active && *id >= 0) {
        *id = -1;
        evdev_report(dev, EV_ABS, ABS_MT_TRACKING_ID, *id);
    }
    if (active) {
        evdev_report(dev, EV_ABS, ABS_MT_TOOL_TYPE, tool);
    }
    return active;
}

/** Clear the marks of slots used in the finished frame. */
void evdev_mt_end_frame(struct evdev *dev)
{
    memset(dev->used, 0, sizeof(dev->used));
}

/** Lift slots absent from the finished frame. */
void evdev_mt_drop_unused(struct evdev *dev)
{
    int i;

    for (i = 0; i < dev->slots; i++) {
        if (dev->id[i] >= 0 && !dev->used[i]) {
            evdev_report(dev, EV_ABS, ABS_MT_SLOT, i);
            evdev_mt_report_slot_state(dev, MT_TOOL_FINGER, 0);
        }
    }
    evdev_mt_end_frame(dev);
}

/** Report whether the model device still has a contact. */
void evdev_mt_report_pointer_emulation(struct evdev *dev, int use_count)
{
    int i;

    (void)use_count;
    for (i = 0; i < dev->slots && dev->id[i] < 0; i++) {
    }
    evdev_report(dev, EV_KEY, BTN_TOUCH, i < dev->slots);
}

/** Mark `dev` taken away. */
void evdev_unregister(struct evdev *dev)
{
    if (dev) {
        dev->gone = 1;
    }
}

/** Return 0: no open holds a grab. */
int evdev_grabbed(struct evdev *dev)
{
    (void)dev;
    return 0;
}

/** Record a single event and count it toward the current packet. */
static void seen_event(struct evdev *dev, uint16_t type, uint16_t code, int32_t value)
{
    dev->pending++;
    if (nseen < MAX_SEEN) {
        seen[nseen].dev = dev;
        seen[nseen].type = type;
        seen[nseen].code = code;
        seen[nseen].value = value;
        nseen++;
    }
}

/** Accept an absolute event, keeping separate state for each MT slot. */
static int model_abs(struct evdev *dev, uint16_t code, int32_t value)
{
    int32_t *old = &dev->absval[code];

    if (!bit(dev->abs, code)) {
        return 0;
    }
    if (code == ABS_MT_SLOT) {
        if (value >= 0 && value < dev->slots) {
            dev->slot = value;
        }
        return 0;
    }
    if (dev->slots && code >= ABS_MT_FIRST && code <= ABS_MT_LAST) {
        old = &dev->slot_value[dev->slot][code - ABS_MT_FIRST];
        if (*old != value && dev->slot != dev->emitted_slot) {
            dev->emitted_slot = dev->slot;
            seen_event(dev, EV_ABS, ABS_MT_SLOT, dev->slot);
        }
    }
    if (*old == value) {
        return 0;
    }
    *old = value;
    return 1;
}

/** Record the event a reader of `dev` would see, after the input core's
 * filters. */
void evdev_report(struct evdev *dev, uint16_t type, uint16_t code, int32_t value)
{
    if (!dev) {
        return;
    }
    switch (type) {
    case EV_KEY:
        if (!bit(dev->keys, code) || bit(dev->down, code) == (value != 0)) {
            return;
        }
        dev->down[code >> 3] ^= (uint8_t)(1u << (code & 7));
        break;
    case EV_REL:
        if (!bit(dev->rel, code) || value == 0) {
            return;
        }
        break;
    case EV_ABS:
        if (!model_abs(dev, code, value)) {
            return;
        }
        break;
    case EV_MSC:
        if (!(dev->flags & EVDEV_SCAN)) {
            return;
        }
        break;
    case EV_SYN:
        if (!dev->pending) {
            return;
        }
        dev->pending = -1;
        break;
    default:
        break;
    }
    seen_event(dev, type, code, value);
}

/** Attach a keyboard as the input layer does. */
void input_keyboard_attach_dev(struct input_keyboard *kb, struct evdev *dev)
{
    kb->dev = dev;
    if (dev) {
        dev->keyboard = 1;
    }
}

/** Take a key as the input layer does: a press of a key held and a release
 * of one not held are dropped; the rest is MSC_SCAN, the key and
 * SYN_REPORT. */
void input_key_event(struct input_keyboard *kb, uint32_t scan, unsigned int keycode, int down)
{
    uint8_t b = (uint8_t)(1u << (keycode & 7));

    if (keycode && (down ? (kb->down[keycode >> 3] & b) != 0 : (kb->down[keycode >> 3] & b) == 0)) {
        return;
    }
    if (keycode) {
        kb->down[keycode >> 3] ^= b;
    }
    evdev_report(kb->dev, EV_MSC, MSC_SCAN, (int32_t)scan);
    if (keycode) {
        evdev_report(kb->dev, EV_KEY, (uint16_t)keycode, down ? 1 : 0);
    }
    evdev_report(kb->dev, EV_SYN, SYN_REPORT, 0);
}

/** Release the keys `kb` holds, as the input layer does. */
void input_keyboard_detach(struct input_keyboard *kb)
{
    unsigned code;

    for (code = 1; code < INPUT_KEY_BYTES * 8; code++) {
        if (kb->down[code >> 3] & (1u << (code & 7))) {
            input_key_event(kb, 0, code, 0);
        }
    }
    kb->dev = NULL;
}

/** Count a mouse found. */
void input_mouse_found(void)
{
    mice_found++;
}

/** Keep the mouse queue's last packet. */
void mouse_push_packet(uint8_t buttons, int dx, int dy)
{
    packets++;
    packet_buttons = buttons;
    packet_dx = dx;
    packet_dy = dy;
}

/** Do nothing: the latency record is not checked. */
void usb_stamp_wake(void)
{}

/** Allocate zero-filled packet workspace for the driver. */
void *kzalloc(size_t size)
{
    return calloc(1, size);
}

/** Release the packet workspace. */
void kfree(void *p)
{
    free(p);
}

#include "../../rach2/sys/usb/hid_parse.c"
#include "../../rach2/sys/usb/hid_input.c"

/* Linux's codes of the keys the rows name that no kernel header does. */
#define KEY_RESERVED  0
#define KEY_LEFTCTRL  29
#define KEY_A         30
#define KEY_LEFTSHIFT 42
#define KEY_B         48
#define KEY_RIGHTMETA 126
#define KEY_PLAYPAUSE 164
#define KEY_HOMEPAGE  172

/* ---- the descriptors ---- */

/* QEMU 11's hw/usb/dev-hid.c. */
static const uint8_t qemu_tablet[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29,
    0x05, 0x15, 0x00, 0x25, 0x01, 0x95, 0x05, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x03,
    0x81, 0x01, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x26, 0xff, 0x7f, 0x35, 0x00,
    0x46, 0xff, 0x7f, 0x75, 0x10, 0x95, 0x02, 0x81, 0x02, 0x05, 0x01, 0x09, 0x38, 0x15, 0x81,
    0x25, 0x7f, 0x35, 0x00, 0x45, 0x00, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06, 0xc0, 0xc0};

static const uint8_t qemu_mouse[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x09, 0x19,
    0x01, 0x29, 0x05, 0x15, 0x00, 0x25, 0x01, 0x95, 0x05, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x03, 0x81, 0x01, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09,
    0x38, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06, 0xc0, 0xc0};

static const uint8_t qemu_keyboard[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x75, 0x01, 0x95, 0x08, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7,
    0x15, 0x00, 0x25, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x05, 0x75, 0x01,
    0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0x95, 0x06,
    0x75, 0x08, 0x15, 0x00, 0x25, 0xff, 0x05, 0x07, 0x19, 0x00, 0x29, 0xff, 0x81, 0x00, 0xc0};

/* A keyboard with a Consumer Control and a System Control application, its
 * reports numbered 1, 2 and 3, the Consumer Control's with AC Pan. */
static const uint8_t media_keyboard[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, 0x01,                         /* Keyboard, ID 1 */
    0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01,             /* modifiers */
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, /* reserved */
    0x05, 0x08, 0x19, 0x01, 0x29, 0x03, 0x95, 0x03, 0x75, 0x01, 0x91, 0x02, /* 3 LEDs */
    0x95, 0x01, 0x75, 0x05, 0x91, 0x01,                                     /* padding */
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x15, 0x00, 0x25, 0x65, 0x95, 0x06, 0x75,
    0x08, 0x81, 0x00, 0xc0,                         /* keys */
    0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x85, 0x02, /* Consumer Control, ID 2 */
    0x09, 0xe2, 0x09, 0xe9, 0x09, 0xea, 0x09, 0xcd, 0x0a, 0x23, 0x02, 0x15, 0x00,
    0x25, 0x01, 0x75, 0x01, 0x95, 0x05, 0x81, 0x02, /* Mute, Vol+, Vol-, Play, Home */
    0x95, 0x03, 0x81, 0x01,                         /* padding */
    0x0a, 0x38, 0x02, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06, /* AC Pan */
    0xc0, 0x05, 0x01, 0x09, 0x80, 0xa1, 0x01, 0x85, 0x03, /* System Control, ID 3 */
    0x19, 0x81, 0x29, 0x82, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x02, 0x81,
    0x02, 0x95, 0x06, 0x81, 0x01, 0xc0};

/* A Consumer Control with Num, Caps and Scroll Lock LEDs and no keys:
 * an event device that is not a keyboard. */
static const uint8_t lamp[] = {0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x05, 0x08, 0x19,
                               0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01,
                               0x95, 0x03, 0x91, 0x02, 0x95, 0x05, 0x91, 0x01, 0xc0};

/* A keyboard and a mouse in one device, two input devices. */
static const uint8_t combo[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, 0x01, 0x05, 0x07, 0x19, 0x04, 0x29, 0x05, 0x15, 0x00,
    0x25, 0x01, 0x75, 0x01, 0x95, 0x02, 0x81, 0x02, 0x95, 0x06, 0x81, 0x01, 0xc0, 0x05, 0x01, 0x09,
    0x02, 0xa1, 0x01, 0x85, 0x02, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15,
    0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x03, 0x81, 0x02, 0x95, 0x05, 0x81, 0x01, 0x05, 0x01, 0x09,
    0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06, 0xc0, 0xc0};

/* A wheel mouse with a Resolution Multiplier of 1 to 8 in the wheel's
 * logical collection, in Feature report 2. */
static const uint8_t hires_mouse[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x01, 0x09, 0x01, 0xa1, 0x00, 0x05,
    0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01,
    0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x01, 0x05, 0x01, 0x09, 0x30, 0x09,
    0x31, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06, 0xa1, 0x02, /* Logical */
    0x85, 0x02, 0x09, 0x48, 0x15, 0x00, 0x25, 0x01, 0x35, 0x01, 0x45, 0x08, 0x75,
    0x02, 0x95, 0x01, 0xb1, 0x02, 0x75, 0x06, 0xb1, 0x01, /* multiplier */
    0x85, 0x01, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x35, 0x00, 0x45, 0x00, 0x75,
    0x08, 0x95, 0x01, 0x81, 0x06, 0xc0, 0xc0, 0xc0}; /* wheel */

/* A vendor-defined application, which input does not handle. */
static const uint8_t vendor_only[] = {0x06, 0x00, 0xff, 0x09, 0x01, 0xa1, 0x01,
                                      0x09, 0x01, 0x15, 0x00, 0x26, 0xff, 0x00,
                                      0x75, 0x08, 0x95, 0x08, 0x81, 0x02, 0xc0};

/* ---- the rows ---- */

/* A gamepad of four buttons, a hat switch of eight directions with a null
 * state, X and Y, and the D-pad's four usages: byte 0 the buttons and the
 * hat, bytes 1 and 2 the axes, byte 3 the D-pad. */
static const uint8_t gamepad[] = {
    0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x05, 0x09, 0x19, 0x01, 0x29, 0x04, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x04, 0x81, 0x02, 0x05, 0x01, 0x09, 0x39, 0x15, 0x00, 0x25, 0x07, 0x75, 0x04,
    0x95, 0x01, 0x81, 0x42, 0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95,
    0x02, 0x81, 0x02, 0x09, 0x90, 0x09, 0x91, 0x09, 0x92, 0x09, 0x93, 0x15, 0x00, 0x25, 0x01, 0x75,
    0x01, 0x95, 0x04, 0x81, 0x02, 0x75, 0x04, 0x95, 0x01, 0x81, 0x03, 0xc0};

/* Gamepads whose hat switch has a range of 0 to 0xFFFFFFFF, which Linux
 * stores backwards, and of INT32_MIN to INT32_MAX, which Linux's 16-bit
 * hat_min and hat_max keep as 0 and -1, so that it takes each for an axis:
 * one byte of buttons, then the hat's four bytes. */
static const uint8_t hat_backwards[] = {0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x05, 0x09, 0x19, 0x01,
                                        0x29, 0x08, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08,
                                        0x81, 0x02, 0x05, 0x01, 0x09, 0x39, 0x15, 0x00, 0x27, 0xff,
                                        0xff, 0xff, 0xff, 0x75, 0x20, 0x95, 0x01, 0x81, 0x02, 0xc0};
static const uint8_t hat_widest[] = {
    0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x05, 0x09, 0x19, 0x01, 0x29, 0x08, 0x17, 0x00, 0x00, 0x00,
    0x80, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x05, 0x01, 0x09, 0x39, 0x17, 0x00, 0x00,
    0x00, 0x80, 0x27, 0xff, 0xff, 0xff, 0x7f, 0x75, 0x20, 0x95, 0x01, 0x81, 0x02, 0xc0};

/* A gamepad with two bytes of the Generic Device Controls page: Battery
 * Strength, then usage 0x21, which Linux takes for an unknown axis. */
static const uint8_t controls[] = {0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x05, 0x06,
                                   0x09, 0x20, 0x09, 0x21, 0x15, 0x00, 0x26, 0xff,
                                   0x00, 0x75, 0x08, 0x95, 0x02, 0x81, 0x02, 0xc0};

/* Linux's ABS_HAT0X, ABS_HAT0Y and ABS_MISC, which hid_input.c defines for
 * itself. */
#define TEST_HAT0X 0x10
#define TEST_HAT0Y 0x11
#define TEST_MISC  0x28

static int fails;

#define CHECK(what, cond)                 \
    do {                                  \
        if (!(cond)) {                    \
            printf("  FAIL: %s\n", what); \
            fails++;                      \
        }                                 \
    } while (0)

/** Parse `rdesc` twice, as hid.c does, into a new `struct hid_dev`. Returns
 * hid_parse's answer; `*out` is the device on 0. */
static int parse(const uint8_t *rdesc, int length, struct hid_dev **out)
{
    struct hid_dev *hid = calloc(1, sizeof(*hid));
    uint32_t i;
    int err;

    *out = NULL;
    err = hid_parse(&hid->desc, rdesc, length);
    if (err) {
        free(hid);
        return err;
    }
    hid->desc.fields = calloc(hid->desc.max_fields + 1, sizeof(struct hid_field));
    hid->desc.usages = calloc(hid->desc.max_usages + 1, sizeof(struct hid_usage));
    hid->desc.collections = calloc(hid->desc.max_collections + 1, sizeof(struct hid_collection));
    err = hid_parse(&hid->desc, rdesc, length);
    if (err) {
        printf("  the second pass refused what the first took: %d\n", err);
        exit(1);
    }
    for (i = 0; i < hid->desc.nreports; i++) {
        const struct hid_report *r = &hid->desc.reports[i];

        if (r->type == HID_INPUT_REPORT && hid_report_len(r) > hid->insize) {
            hid->insize = hid_report_len(r);
        }
        if (r->type == HID_FEATURE_REPORT && hid_report_len(r) > hid->outsize) {
            hid->outsize = hid_report_len(r);
        }
    }
    hid->inbuf = calloc(hid->insize + 1, 1);
    hid->outbuf = calloc(hid->outsize + 1, 1);
    *out = hid;
    return 0;
}

/** Parse `rdesc` and connect it as "`name`" from QEMU's IDs. */
static struct hid_dev *connect(const uint8_t *rdesc, int length, const char *name, int *inputs)
{
    struct hid_dev *hid;

    if (parse(rdesc, length, &hid) != 0) {
        printf("  FAIL: %s does not parse\n", name);
        exit(1);
    }
    snprintf(hid->name, sizeof(hid->name), "%s", name);
    hid->phys = "usb-1/input0";
    hid->vendor = 0x0627;
    hid->product = 0x0001;
    hid->version = 0x0001;
    *inputs = hidinput_connect(hid);
    return hid;
}

/** Take `hid` away and free it. */
static void drop(struct hid_dev *hid)
{
    hidinput_disconnect(hid);
    free(hid->desc.fields);
    free(hid->desc.usages);
    free(hid->desc.collections);
    free(hid->inbuf);
    free(hid->outbuf);
    free(hid);
}

/** The parse's answer to `rdesc` alone. */
static int verdict(const uint8_t *rdesc, int length)
{
    struct hid_dev *hid;
    int err = parse(rdesc, length, &hid);

    if (!err) {
        drop(hid);
    }
    return err;
}

/** Feed one report and keep the events it gave. */
static void feed(struct hid_dev *hid, const uint8_t *report, uint32_t size)
{
    nseen = 0;
    hid_input_report(hid, report, size);
}

/** Whether event `i` seen is `type`, `code`, `value`. */
static int is(int i, unsigned type, unsigned code, int32_t value)
{
    return i < nseen && seen[i].type == type && seen[i].code == code && seen[i].value == value;
}

/** The keys set in `keys`. */
static int count_bits(const uint8_t *keys, int bytes)
{
    int i, n = 0;

    for (i = 0; i < bytes * 8; i++) {
        n += bit(keys, (unsigned)i);
    }
    return n;
}

static void rows_tablet_axes(struct hid_dev *hid);

/** QEMU's tablet parsed: its report, fields and collections. */
static void rows_parse_tablet(void)
{
    struct hid_dev *hid;
    struct hid_field *f;

    CHECK("tablet: parses", parse(qemu_tablet, sizeof(qemu_tablet), &hid) == 0);
    CHECK("tablet: one input report, unnumbered, of 6 bytes",
          hid->desc.nreports == 1 && hid->desc.reports[0].type == HID_INPUT_REPORT &&
              hid->desc.reports[0].id == 0 && hid_report_len(&hid->desc.reports[0]) == 6 &&
              !hid->desc.numbered[HID_INPUT_REPORT]);
    CHECK("tablet: the padding makes no field", hid->desc.nfields == 3);
    f = &hid->desc.fields[0];
    CHECK("tablet: five buttons of a bit at bit 0",
          f->offset == 0 && f->size == 1 && f->count == 5 && f->maxusage == 5 &&
              (f->flags & HID_MAIN_ITEM_VARIABLE) && hid->desc.usages[f->usage].hid == 0x90001 &&
              hid->desc.usages[f->usage + 4].hid == 0x90005);
    CHECK("tablet: in a mouse application and a pointer collection",
          f->application == 0x10002 && f->physical == 0x10001 && hid->desc.maxapplication == 1);
    rows_tablet_axes(hid);
    drop(hid);
}

/** The axes and collections of QEMU's tablet, `hid`. */
static void rows_tablet_axes(struct hid_dev *hid)
{
    struct hid_field *f = &hid->desc.fields[1];

    CHECK("tablet: X and Y, 16 bits at bit 8, 0 to 32767",
          f->offset == 8 && f->size == 16 && f->count == 2 && f->logical_minimum == 0 &&
              f->logical_maximum == 32767 && f->physical_maximum == 32767 &&
              !(f->flags & HID_MAIN_ITEM_RELATIVE) && hid->desc.usages[f->usage].hid == 0x10030 &&
              hid->desc.usages[f->usage + 1].hid == 0x10031);
    f = &hid->desc.fields[2];
    CHECK("tablet: a relative wheel of a byte at bit 40, -127 to 127",
          f->offset == 40 && f->size == 8 && f->count == 1 && f->logical_minimum == -127 &&
              f->logical_maximum == 127 && (f->flags & HID_MAIN_ITEM_RELATIVE) &&
              hid->desc.usages[f->usage].hid == 0x10038);
    CHECK("tablet: two collections, the pointer in the mouse",
          hid->desc.ncollections == 2 && hid->desc.collections[0].usage == 0x10002 &&
              hid->desc.collections[0].type == HID_COLLECTION_APPLICATION &&
              hid->desc.collections[1].parent == 0 && hid->desc.collections[1].level == 1);
}

/** QEMU's mouse and keyboard parsed. */
static void rows_parse_keyboards(void)
{
    struct hid_dev *hid;
    struct hid_field *f;

    CHECK("mouse: parses", parse(qemu_mouse, sizeof(qemu_mouse), &hid) == 0);
    f = &hid->desc.fields[1];
    CHECK("mouse: X, Y and wheel, relative bytes at bit 8, in a report of 4 bytes",
          hid->desc.nfields == 2 && f->offset == 8 && f->size == 8 && f->count == 3 &&
              (f->flags & HID_MAIN_ITEM_RELATIVE) && hid_report_len(&hid->desc.reports[0]) == 4);
    drop(hid);

    CHECK("keyboard: parses", parse(qemu_keyboard, sizeof(qemu_keyboard), &hid) == 0);
    CHECK("keyboard: an input report of 8 bytes and an output report of 1",
          hid->desc.nreports == 2 &&
              hid_report_len(hid_find_report(&hid->desc, HID_INPUT_REPORT, 0)) == 8 &&
              hid_report_len(hid_find_report(&hid->desc, HID_OUTPUT_REPORT, 0)) == 1);
    f = &hid->desc.fields[0];
    CHECK("keyboard: eight modifiers, usages 0xe0 to 0xe7",
          f->count == 8 && f->size == 1 && hid->desc.usages[f->usage].hid == 0x700e0 &&
              hid->desc.usages[f->usage + 7].hid == 0x700e7);
    f = &hid->desc.fields[1];
    CHECK("keyboard: five LEDs in the output report",
          hid->desc.reports[f->report].type == HID_OUTPUT_REPORT && f->count == 5 &&
              hid->desc.usages[f->usage].hid == 0x80001);
    f = &hid->desc.fields[2];
    CHECK("keyboard: an array of six key bytes at bit 16 over 256 usages",
          f->offset == 16 && f->size == 8 && f->count == 6 && f->maxusage == 256 &&
              !(f->flags & HID_MAIN_ITEM_VARIABLE) && f->logical_maximum == 255);
    drop(hid);
}

/** A keyboard of three applications and numbered reports parsed. */
static void rows_parse_media(void)
{
    struct hid_dev *hid;

    CHECK("media keyboard: parses", parse(media_keyboard, sizeof(media_keyboard), &hid) == 0);
    CHECK("media keyboard: numbered input reports 1, 2 and 3 and output report 1",
          hid->desc.numbered[HID_INPUT_REPORT] && hid->desc.numbered[HID_OUTPUT_REPORT] &&
              hid_find_report(&hid->desc, HID_INPUT_REPORT, 1) &&
              hid_find_report(&hid->desc, HID_INPUT_REPORT, 2) &&
              hid_find_report(&hid->desc, HID_INPUT_REPORT, 3) &&
              hid_find_report(&hid->desc, HID_OUTPUT_REPORT, 1) &&
              !hid_find_report(&hid->desc, HID_INPUT_REPORT, 4));
    CHECK("media keyboard: report 1 is 9 bytes with its ID, report 2 is 3",
          hid_report_len(hid_find_report(&hid->desc, HID_INPUT_REPORT, 1)) == 9 &&
              hid_report_len(hid_find_report(&hid->desc, HID_INPUT_REPORT, 2)) == 3);
    CHECK("media keyboard: three applications", hid->desc.maxapplication == 3);
    CHECK("media keyboard: report 2 is the Consumer Control's",
          hid_find_report(&hid->desc, HID_INPUT_REPORT, 2)->application == 0xc0001);
    drop(hid);
}

/** Descriptors Linux refuses, and two it takes. */
static void rows_parse_malformed(void)
{
    static const uint8_t empty[1] = {0};
    static const uint8_t cut[] = {0x05, 0x01, 0x26, 0xff};
    static const uint8_t long_item[] = {0xfe, 0x02, 0x10, 0x00, 0x00};
    static const uint8_t big_size[] = {0x09, 0x01, 0x76, 0x01, 0x01, 0x95, 0x01, 0x81, 0x02};
    static const uint8_t big_count[] = {0x09, 0x01, 0x75, 0x01, 0x96, 0x01, 0x30, 0x81, 0x02};
    static const uint8_t id_zero[] = {0x85, 0x00};
    static const uint8_t id_big[] = {0x86, 0x00, 0x01};
    static const uint8_t lone_end[] = {0xc0};
    static const uint8_t open[] = {0x05, 0x01, 0x09, 0x02, 0xa1, 0x01};
    static const uint8_t pushes[] = {0xa4, 0xa4, 0xa4, 0xa4, 0xa4};
    static const uint8_t four_pushes[] = {0xa4, 0xa4, 0xa4, 0xa4, 0xb4, 0xb4, 0xb4, 0xb4,
                                          0x09, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02};
    static const uint8_t pop[] = {0xb4};
    static const uint8_t range[] = {0x09, 0x01, 0x15, 0x05, 0x25, 0x01,
                                    0x75, 0x08, 0x95, 0x01, 0x81, 0x02};
    static const uint8_t signed_range[] = {0x09, 0x01, 0x15, 0xff, 0x25, 0x01,
                                           0x75, 0x08, 0x95, 0x01, 0x81, 0x02};
    static const uint8_t global12[] = {0xc4};
    static const uint8_t too_long[] = {0x09, 0x01, 0x76, 0x00, 0x01, 0x96, 0xe8, 0x03, 0x81, 0x02};
    static const uint8_t delim[] = {0xa9, 0x00};
    static const uint8_t nested_delim[] = {0xa9, 0x01, 0xa9, 0x01, 0xa9, 0x00, 0xa9, 0x00};
    static const uint8_t open_delim[] = {0xa9, 0x01, 0x09, 0x01};
    static uint8_t wide[] = {0x05, 0x01, 0x19, 0x00, 0x2a, 0xff, 0xff,
                             0x75, 0x01, 0x95, 0x01, 0x81, 0x00};
    struct hid_dev *hid;

    CHECK("malformed: an empty descriptor is refused", verdict(empty, 0) == -EINVAL);
    CHECK("malformed: an item cut short is refused", verdict(cut, sizeof(cut)) == -EINVAL);
    CHECK("malformed: a long item is refused", verdict(long_item, sizeof(long_item)) == -EINVAL);
    CHECK("malformed: a Report Size of 257 is refused",
          verdict(big_size, sizeof(big_size)) == -EINVAL);
    CHECK("malformed: a Report Count of 12289 is refused",
          verdict(big_count, sizeof(big_count)) == -EINVAL);
    CHECK("malformed: Report ID 0 is refused", verdict(id_zero, sizeof(id_zero)) == -EINVAL);
    CHECK("malformed: Report ID 256 is refused", verdict(id_big, sizeof(id_big)) == -EINVAL);
    CHECK("malformed: an End Collection with nothing open is refused",
          verdict(lone_end, sizeof(lone_end)) == -EINVAL);
    CHECK("malformed: a descriptor that ends in a collection is refused",
          verdict(open, sizeof(open)) == -EINVAL);
    CHECK("malformed: a fifth Push is refused", verdict(pushes, sizeof(pushes)) == -EINVAL);
    CHECK("malformed: four Pushes and four Pops are taken",
          verdict(four_pushes, sizeof(four_pushes)) == 0);
    CHECK("malformed: a Pop of nothing is refused", verdict(pop, sizeof(pop)) == -EINVAL);
    CHECK("malformed: a logical maximum below the minimum is refused",
          verdict(range, sizeof(range)) == -EINVAL);
    CHECK("malformed: -1 to 1 is a range", verdict(signed_range, sizeof(signed_range)) == 0);
    CHECK("malformed: global item 12 is refused", verdict(global12, sizeof(global12)) == -EINVAL);
    CHECK("malformed: a report over 16383 bytes is refused",
          verdict(too_long, sizeof(too_long)) == -EINVAL);
    CHECK("malformed: a delimiter closed with none open is refused",
          verdict(delim, sizeof(delim)) == -EINVAL);
    CHECK("malformed: a nested delimiter is refused",
          verdict(nested_delim, sizeof(nested_delim)) == -EINVAL);
    CHECK("malformed: a descriptor that ends in a delimiter is refused",
          verdict(open_delim, sizeof(open_delim)) == -EINVAL);

    CHECK("malformed: a usage range past 12288 is clipped", parse(wide, sizeof(wide), &hid) == 0);
    CHECK("malformed: to 12288 usages",
          hid->desc.nfields == 1 && hid->desc.fields[0].maxusage == HID_MAX_USAGES);
    drop(hid);
}

/** 300 one-bit fields: Linux keeps the first 256, and the report its 300
 * bits. */
static void rows_parse_fields(void)
{
    static uint8_t many[4 + 300 * 4];
    struct hid_dev *hid;
    int i, n = 0;

    many[n++] = 0x75; /* Report Size (1) */
    many[n++] = 0x01;
    many[n++] = 0x95; /* Report Count (1) */
    many[n++] = 0x01;
    for (i = 0; i < 300; i++) {
        many[n++] = 0x09; /* Usage (1) */
        many[n++] = 0x01;
        many[n++] = 0x81; /* Input (Variable) */
        many[n++] = 0x02;
    }
    CHECK("fields: 300 fields in one report parse", parse(many, n, &hid) == 0);
    CHECK("fields: the first 256 are kept", hid->desc.nfields == HID_MAX_FIELDS);
    CHECK("fields: the report keeps its 300 bits", hid->desc.reports[0].size == 300);
    drop(hid);
}

static void rows_tablet_events(struct hid_dev *hid);

/** QEMU's tablet as an event device. */
static void rows_map_tablet(void)
{
    struct hid_dev *hid;
    struct evdev *d;
    int inputs, code, ok = 1;

    ndevs = 0;
    mice_found = 0;
    hid = connect(qemu_tablet, sizeof(qemu_tablet), "QEMU QEMU USB Tablet", &inputs);
    d = &devs[0];
    CHECK("tablet: one event device", inputs == 1 && ndevs == 1);
    CHECK("tablet: named QEMU QEMU USB Tablet on usb-1/input0",
          strcmp(d->name, "QEMU QEMU USB Tablet") == 0 && strcmp(d->phys, "usb-1/input0") == 0);
    CHECK("tablet: bus 3, 0627:0001, version 1",
          d->bus == 3 && d->vendor == 0x0627 && d->product == 0x0001 && d->version == 1);
    for (code = 0; code < KEY_CNT; code++) {
        ok &= bit(d->keys, (unsigned)code) == (code >= BTN_LEFT && code <= BTN_EXTRA);
    }
    CHECK("tablet: the keys are BTN_LEFT to BTN_EXTRA", ok);
    CHECK("tablet: REL_WHEEL and REL_WHEEL_HI_RES alone",
          bit(d->rel, REL_WHEEL) && bit(d->rel, REL_WHEEL_HI_RES) && count_bits(d->rel, 2) == 2);
    CHECK("tablet: ABS_X and ABS_Y alone",
          bit(d->abs, ABS_X) && bit(d->abs, ABS_Y) && count_bits(d->abs, 8) == 2);
    CHECK("tablet: over 0 to 32767, no fuzz, flat or resolution",
          d->absmin[ABS_X] == 0 && d->absmax[ABS_X] == 32767 && d->absmin[ABS_Y] == 0 &&
              d->absmax[ABS_Y] == 32767 && !d->fuzz[ABS_X] && !d->flat[ABS_X] && !d->res[ABS_X] &&
              !d->res[ABS_Y]);
    CHECK("tablet: MSC_SCAN, no LEDs, not a keyboard",
          (d->flags & EVDEV_SCAN) && !(d->flags & EVDEV_KEYBOARD) && count_bits(d->led, 2) == 0 &&
              !d->keyboard);
    CHECK("tablet: not a mouse of the mouse queue", mice_found == 0);
    rows_tablet_events(hid);
    drop(hid);
    CHECK("tablet: its event device is taken away", d->gone);
}

/** The events of QEMU's tablet, `hid`, as evtest prints Linux's. */
static void rows_tablet_events(struct hid_dev *hid)
{
    static const uint8_t move[] = {0x00, 0x00, 0x10, 0x00, 0x20, 0x00};
    static const uint8_t press[] = {0x01, 0x00, 0x10, 0x00, 0x20, 0x00};
    static const uint8_t wheel[] = {0x01, 0x00, 0x10, 0x00, 0x20, 0x01};
    static const uint8_t back[] = {0x01, 0x00, 0x10, 0x00, 0x20, 0xff};
    static const uint8_t release[] = {0x00, 0x00, 0x10, 0x00, 0x20, 0x00};
    static const uint8_t side[] = {0x18, 0x00, 0x10, 0x00, 0x20, 0x00};
    static const uint8_t short_report[] = {0x00, 0xff, 0x7f};

    feed(hid, move, sizeof(move));
    CHECK("tablet: a move is ABS_X, ABS_Y and SYN_REPORT",
          nseen == 3 && is(0, EV_ABS, ABS_X, 4096) && is(1, EV_ABS, ABS_Y, 8192) &&
              is(2, EV_SYN, SYN_REPORT, 0));
    feed(hid, press, sizeof(press));
    CHECK("tablet: a click is MSC_SCAN 0x90001, BTN_LEFT and SYN_REPORT",
          nseen == 3 && is(0, EV_MSC, MSC_SCAN, 0x90001) && is(1, EV_KEY, BTN_LEFT, 1) &&
              is(2, EV_SYN, SYN_REPORT, 0));
    feed(hid, press, sizeof(press));
    CHECK("tablet: the same report again gives nothing", nseen == 0);
    feed(hid, wheel, sizeof(wheel));
    CHECK("tablet: a wheel's detent is REL_WHEEL 1 and REL_WHEEL_HI_RES 120",
          nseen == 3 && is(0, EV_REL, REL_WHEEL, 1) && is(1, EV_REL, REL_WHEEL_HI_RES, 120) &&
              is(2, EV_SYN, SYN_REPORT, 0));
    feed(hid, back, sizeof(back));
    CHECK("tablet: and back, -1 and -120",
          nseen == 3 && is(0, EV_REL, REL_WHEEL, -1) && is(1, EV_REL, REL_WHEEL_HI_RES, -120));
    feed(hid, release, sizeof(release));
    CHECK("tablet: a release is MSC_SCAN 0x90001 and BTN_LEFT 0",
          nseen == 3 && is(0, EV_MSC, MSC_SCAN, 0x90001) && is(1, EV_KEY, BTN_LEFT, 0));
    feed(hid, side, sizeof(side));
    CHECK("tablet: buttons 4 and 5 are BTN_SIDE and BTN_EXTRA",
          nseen == 5 && is(0, EV_MSC, MSC_SCAN, 0x90004) && is(1, EV_KEY, BTN_SIDE, 1) &&
              is(2, EV_MSC, MSC_SCAN, 0x90005) && is(3, EV_KEY, BTN_EXTRA, 1));
    feed(hid, short_report, sizeof(short_report));
    CHECK("tablet: a short report reads as if padded with zeros",
          nseen == 7 && is(4, EV_ABS, ABS_X, 32767) && is(5, EV_ABS, ABS_Y, 0));
    feed(hid, short_report, 0);
    CHECK("tablet: a report of no bytes is dropped", nseen == 0);
    CHECK("tablet: and nothing reached the mouse queue", packets == 0);
}

/** QEMU's mouse as an event device, and its events. */
static void rows_map_mouse(void)
{
    static const uint8_t move[] = {0x02, 0x05, 0xfd, 0x00};
    static const uint8_t still[] = {0x02, 0x00, 0x00, 0x00};
    struct hid_dev *hid;
    struct evdev *d;
    int inputs;

    ndevs = 0;
    mice_found = 0;
    packets = 0;
    hid = connect(qemu_mouse, sizeof(qemu_mouse), "QEMU QEMU USB Mouse", &inputs);
    d = &devs[0];
    CHECK("mouse: one event device named QEMU QEMU USB Mouse",
          inputs == 1 && strcmp(d->name, "QEMU QEMU USB Mouse") == 0);
    CHECK("mouse: REL_X, REL_Y, REL_WHEEL and REL_WHEEL_HI_RES",
          bit(d->rel, REL_X) && bit(d->rel, REL_Y) && bit(d->rel, REL_WHEEL) &&
              bit(d->rel, REL_WHEEL_HI_RES) && count_bits(d->rel, 2) == 4);
    CHECK("mouse: five buttons and no axis",
          count_bits(d->keys, EVDEV_KEY_BYTES) == 5 && count_bits(d->abs, 8) == 0);
    CHECK("mouse: found for the mouse queue", mice_found == 1);
    feed(hid, move, sizeof(move));
    CHECK("mouse: MSC_SCAN, BTN_RIGHT, REL_X 5, REL_Y -3, SYN_REPORT",
          nseen == 5 && is(0, EV_MSC, MSC_SCAN, 0x90002) && is(1, EV_KEY, BTN_RIGHT, 1) &&
              is(2, EV_REL, REL_X, 5) && is(3, EV_REL, REL_Y, -3) && is(4, EV_SYN, SYN_REPORT, 0));
    CHECK("mouse: the mouse queue has its packet, up positive",
          packets == 1 && packet_buttons == 2 && packet_dx == 5 && packet_dy == 3);
    feed(hid, still, sizeof(still));
    CHECK("mouse: a report of no change gives no event", nseen == 0);
    drop(hid);
}

static void rows_keyboard_leds(struct hid_dev *hid);
static void rows_keyboard_keymap(struct hid_dev *hid, struct evdev *d);

/** QEMU's keyboard as an event device, and its keys. */
static void rows_map_keyboard(void)
{
    static const uint8_t shift_a[] = {0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00};
    static const uint8_t a_b[] = {0x00, 0x00, 0x05, 0x04, 0x00, 0x00, 0x00, 0x00};
    static const uint8_t rollover[] = {0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01};
    static const uint8_t none[8] = {0};
    struct hid_dev *hid;
    struct evdev *d;
    int inputs;

    ndevs = 0;
    hid = connect(qemu_keyboard, sizeof(qemu_keyboard), "QEMU QEMU USB Keyboard", &inputs);
    d = &devs[0];
    CHECK("keyboard: one event device named QEMU QEMU USB Keyboard",
          inputs == 1 && strcmp(d->name, "QEMU QEMU USB Keyboard") == 0);
    CHECK("keyboard: attached as a keyboard, with EV_REP and MSC_SCAN",
          d->keyboard && (d->flags & EVDEV_KEYBOARD) && (d->flags & EVDEV_SCAN));
    CHECK("keyboard: Linux's 163 keys",
          count_bits(d->keys, EVDEV_KEY_BYTES) == 163 && bit(d->keys, KEY_A) &&
              bit(d->keys, KEY_ENTER) && bit(d->keys, KEY_LEFTCTRL) &&
              bit(d->keys, KEY_RIGHTMETA) && bit(d->keys, KEY_UNKNOWN) &&
              !bit(d->keys, KEY_RESERVED));
    CHECK("keyboard: the LEDs Num Lock to Kana",
          bit(d->led, LED_NUML) && bit(d->led, LED_CAPSL) && bit(d->led, LED_SCROLLL) &&
              bit(d->led, LED_COMPOSE) && bit(d->led, LED_KANA) && count_bits(d->led, 2) == 5);
    CHECK("keyboard: not a mouse", !hid->inputs[0].mouse);

    feed(hid, shift_a, sizeof(shift_a));
    CHECK("keyboard: Shift and A, each with its usage as MSC_SCAN",
          nseen == 6 && is(0, EV_MSC, MSC_SCAN, 0x700e1) && is(1, EV_KEY, KEY_LEFTSHIFT, 1) &&
              is(3, EV_MSC, MSC_SCAN, 0x70004) && is(4, EV_KEY, KEY_A, 1));
    feed(hid, a_b, sizeof(a_b));
    CHECK("keyboard: Shift up and B down, A held in another slot",
          nseen == 6 && is(1, EV_KEY, KEY_LEFTSHIFT, 0) && is(4, EV_KEY, KEY_B, 1));
    feed(hid, rollover, sizeof(rollover));
    CHECK("keyboard: ErrorRollOver leaves the keys as they were", nseen == 0);
    feed(hid, none, sizeof(none));
    CHECK("keyboard: then all up: B and A",
          nseen == 6 && is(1, EV_KEY, KEY_B, 0) && is(4, EV_KEY, KEY_A, 0));
    rows_keyboard_keymap(hid, d);
    rows_keyboard_leds(hid);
    drop(hid);
}

/** The keymap of QEMU's keyboard `hid`, whose event device is `d`: its
 * usages are its scan codes, as hid-input numbers them. */
static void rows_keyboard_keymap(struct hid_dev *hid, struct evdev *d)
{
    static const uint8_t shift_a[] = {0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00};
    static const uint8_t none[8] = {0};
    struct evdev_keymap_entry ke;
    uint16_t old = 0;

    memset(&ke, 0, sizeof(ke));
    ke.scancode = 0x70004;
    CHECK("keymap: usage 0x70004 is KEY_A", d->km.get(d->km.ctx, &ke) == 0 && ke.keycode == KEY_A);
    ke.by_index = 1;
    ke.index = 0;
    CHECK("keymap: the first entry is the first modifier, Left Control",
          d->km.get(d->km.ctx, &ke) == 0 && ke.scancode == 0x700e0 && ke.keycode == KEY_LEFTCTRL);
    ke.by_index = 0;
    ke.scancode = 0x70004;
    ke.keycode = KEY_B;
    CHECK("keymap: usage 0x70004 is set to KEY_B, and no usage is KEY_A",
          d->km.set(d->km.ctx, &ke, &old) == 0 && old == KEY_A && !d->km.maps(d->km.ctx, KEY_A) &&
              d->km.maps(d->km.ctx, KEY_B));
    feed(hid, shift_a, sizeof(shift_a));
    CHECK("keymap: and the A key reports KEY_B", is(4, EV_KEY, KEY_B, 1));
    feed(hid, none, sizeof(none));
    ke.keycode = KEY_A;
    d->km.set(d->km.ctx, &ke, &old);
    ke.scancode = 0x12345;
    CHECK("keymap: a usage the device has not is -EINVAL", d->km.get(d->km.ctx, &ke) == -EINVAL);
}

/** The output reports of QEMU's keyboard, `hid`. */
static void rows_keyboard_leds(struct hid_dev *hid)
{
    uint8_t out[8];
    int value = 0;

    CHECK("keyboard: Num Lock and Caps Lock are report 0x03",
          hidinput_led_report(hid, (1u << LED_NUML) | (1u << LED_CAPSL), out, sizeof(out),
                              &value) == 1 &&
              out[0] == 0x03 && value == 0x0200);
    CHECK("keyboard: the console's Kana bit is not sent",
          hidinput_led_report(hid, 1u << LED_KANA, out, sizeof(out), &value) == 1 &&
              out[0] == 0x00);
    hid->inputs[0].dev->lit = 1u << LED_CAPSL | 1u << LED_KANA;
    CHECK("keyboard: a program's Kana LED is bit 4, Caps Lock the console's",
          hidinput_led_report(hid, 1u << LED_NUML, out, sizeof(out), &value) == 1 &&
              out[0] == 0x11);
    hid->inputs[0].dev->lit = 0;
    CHECK("keyboard: no multiplier report",
          hidinput_multiplier_report(hid, out, sizeof(out), &value) == -1);
}

/** The keyboard of three applications as one event device. */
static void rows_map_media(void)
{
    static const uint8_t volume_up[] = {0x02, 0x02, 0x00};
    static const uint8_t volume_off[] = {0x02, 0x00, 0x00};
    static const uint8_t pan[] = {0x02, 0x00, 0x02};
    static const uint8_t power[] = {0x03, 0x01};
    static const uint8_t unknown_id[] = {0x07, 0x01};
    uint8_t out[8];
    struct hid_dev *hid;
    struct evdev *d;
    int inputs, value = 0;

    ndevs = 0;
    hid = connect(media_keyboard, sizeof(media_keyboard), "ACME Keys", &inputs);
    d = &devs[0];
    CHECK("media: the Consumer and System Controls join the keyboard",
          inputs == 1 && ndevs == 1 && d->keyboard);
    CHECK("media: named with the keyboard's suffix", strcmp(d->name, "ACME Keys Keyboard") == 0);
    CHECK("media: Mute, Volume Up, Volume Down, Play/Pause and Home",
          bit(d->keys, KEY_MUTE) && bit(d->keys, KEY_VOLUMEUP) && bit(d->keys, KEY_VOLUMEDOWN) &&
              bit(d->keys, KEY_PLAYPAUSE) && bit(d->keys, KEY_HOMEPAGE));
    CHECK("media: System Control's Power and Sleep",
          bit(d->keys, KEY_POWER) && bit(d->keys, KEY_SLEEP));
    CHECK("media: AC Pan is REL_HWHEEL and REL_HWHEEL_HI_RES",
          bit(d->rel, REL_HWHEEL) && bit(d->rel, REL_HWHEEL_HI_RES));
    feed(hid, volume_up, sizeof(volume_up));
    CHECK("media: Volume Up with usage 0xc00e9",
          nseen == 3 && is(0, EV_MSC, MSC_SCAN, 0xc00e9) && is(1, EV_KEY, KEY_VOLUMEUP, 1));
    feed(hid, volume_off, sizeof(volume_off));
    CHECK("media: and up", nseen == 3 && is(1, EV_KEY, KEY_VOLUMEUP, 0));
    feed(hid, pan, sizeof(pan));
    CHECK("media: two steps of pan",
          nseen == 3 && is(0, EV_REL, REL_HWHEEL, 2) && is(1, EV_REL, REL_HWHEEL_HI_RES, 240));
    feed(hid, power, sizeof(power));
    CHECK("media: Power with usage 0x10081",
          nseen == 3 && is(0, EV_MSC, MSC_SCAN, 0x10081) && is(1, EV_KEY, KEY_POWER, 1));
    feed(hid, unknown_id, sizeof(unknown_id));
    CHECK("media: a report of an unknown ID is dropped", nseen == 0);
    CHECK("media: the LED report carries its ID",
          hidinput_led_report(hid, 1u << LED_CAPSL, out, sizeof(out), &value) == 2 &&
              out[0] == 0x01 && out[1] == 0x02 && value == 0x0201);
    CHECK("media: an LED report longer than the buffer is refused",
          hidinput_led_report(hid, 0, out, 1, &value) == -1);
    drop(hid);
}

/** Applications that make two event devices, one and none. */
static void rows_map_apps(void)
{
    struct hid_dev *hid;
    uint8_t out[8];
    int inputs, value = 0;

    ndevs = 0;
    hid = connect(combo, sizeof(combo), "ACME Combo", &inputs);
    CHECK("combo: a keyboard and a mouse are two event devices", inputs == 2 && ndevs == 2);
    CHECK("combo: named by their applications", strcmp(devs[0].name, "ACME Combo Keyboard") == 0 &&
                                                    strcmp(devs[1].name, "ACME Combo Mouse") == 0);
    CHECK("combo: the keyboard has A and B, the mouse three buttons and REL_X and REL_Y",
          devs[0].keyboard && count_bits(devs[0].keys, EVDEV_KEY_BYTES) == 2 && !devs[1].keyboard &&
              count_bits(devs[1].keys, EVDEV_KEY_BYTES) == 3 && bit(devs[1].rel, REL_X) &&
              bit(devs[1].rel, REL_Y));
    drop(hid);

    ndevs = 0;
    hid = connect(combo, sizeof(combo), "ACME Keyboard", &inputs);
    CHECK("combo: a name that ends with the suffix is kept",
          strcmp(devs[0].name, "ACME Keyboard") == 0 &&
              strcmp(devs[1].name, "ACME Keyboard Mouse") == 0);
    drop(hid);

    ndevs = 0;
    hid = connect(vendor_only, sizeof(vendor_only), "ACME Gadget", &inputs);
    CHECK("vendor: a vendor application makes no event device", inputs == -1 && ndevs == 0);
    drop(hid);

    ndevs = 0;
    hid = connect(lamp, sizeof(lamp), "ACME Lamp", &inputs);
    CHECK("lamp: three lock LEDs and no keys make an event device that is not a keyboard",
          inputs == 1 && ndevs == 1 && !devs[0].keyboard);
    hid->inputs[0].dev->lit = 1u << LED_CAPSL;
    CHECK("lamp: its LEDs are those a program lit, not the console's locks",
          hidinput_led_report(hid, (1u << LED_NUML) | (1u << LED_SCROLLL), out, sizeof(out),
                              &value) == 1 &&
              out[0] == 0x02 && value == 0x0200);
    hid->inputs[0].dev->lit = 0;
    drop(hid);
}

/** A wheel with a Resolution Multiplier. */
static void rows_multiplier(void)
{
    static const uint8_t turn[] = {0x01, 0x00, 0x00, 0x00, 0x01};
    uint8_t out[8];
    struct hid_dev *hid;
    int inputs, value = 0, i, detents = 0, hi = 0;

    ndevs = 0;
    hid = connect(hires_mouse, sizeof(hires_mouse), "ACME Wheel", &inputs);
    CHECK("multiplier: one event device with both wheels",
          inputs == 1 && bit(devs[0].rel, REL_WHEEL) && bit(devs[0].rel, REL_WHEEL_HI_RES));
    feed(hid, turn, sizeof(turn));
    CHECK("multiplier: before it is set a step is a detent",
          is(0, EV_REL, REL_WHEEL, 1) && is(1, EV_REL, REL_WHEEL_HI_RES, 120));
    CHECK("multiplier: its report sets it to its maximum",
          hidinput_multiplier_report(hid, out, sizeof(out), &value) == 2 && out[0] == 0x02 &&
              out[1] == 0x01 && value == 0x0302);
    hidinput_multiplier_set(hid, 1);
    for (i = 0; i < 8; i++) {
        feed(hid, turn, sizeof(turn));
        detents += nseen == 3 && is(0, EV_REL, REL_WHEEL, 1);
        hi += is(nseen - 2, EV_REL, REL_WHEEL_HI_RES, 15);
    }
    CHECK("multiplier: once set a step is 15 120ths", hi == 8);
    CHECK("multiplier: and eight steps one detent", detents == 1);
    drop(hid);
}

/** Whether an event seen is `type`, `code`, `value`. */
static int saw(unsigned type, unsigned code, int32_t value)
{
    int i;

    for (i = 0; i < nseen; i++) {
        if (is(i, type, code, value)) {
            return 1;
        }
    }
    return 0;
}

/** A gamepad's hat switch and D-pad, and hat switches of degenerate
 * ranges. */
static void rows_gamepad(void)
{
    static const uint8_t up[] = {0x00, 0x80, 0x80, 0x00};
    static const uint8_t right[] = {0x20, 0x80, 0x80, 0x00};
    static const uint8_t centred[] = {0x80, 0x80, 0x80, 0x00};
    static const uint8_t dpad_up[] = {0x80, 0x80, 0x80, 0x01};
    static const uint8_t zero[] = {0x00, 0x00, 0x00, 0x00, 0x00};
    static const uint8_t three[] = {0x00, 0x03, 0x00, 0x00, 0x00};
    struct hid_dev *hid;
    struct evdev *d;
    int inputs;

    ndevs = 0;
    hid = connect(gamepad, sizeof(gamepad), "ACME Pad", &inputs);
    d = &devs[0];
    CHECK("gamepad: one event device with ABS_HAT0X and ABS_HAT0Y over -1 to 1",
          inputs == 1 && bit(d->abs, TEST_HAT0X) && bit(d->abs, TEST_HAT0Y) &&
              d->absmin[TEST_HAT0X] == -1 && d->absmax[TEST_HAT0X] == 1 &&
              d->absmin[TEST_HAT0Y] == -1 && d->absmax[TEST_HAT0Y] == 1);
    CHECK("gamepad: the D-pad's usages map to nothing, as Linux 6.12's SystemControl case "
          "ignores 0x90 to 0x9f before its D-pad case",
          count_bits(d->abs, 8) == 4);
    feed(hid, up, sizeof(up));
    CHECK("gamepad: hat 0 is up", saw(EV_ABS, TEST_HAT0Y, -1) && !saw(EV_ABS, TEST_HAT0X, 1));
    feed(hid, right, sizeof(right));
    CHECK("gamepad: hat 2 is right", saw(EV_ABS, TEST_HAT0X, 1) && saw(EV_ABS, TEST_HAT0Y, 0));
    feed(hid, centred, sizeof(centred));
    CHECK("gamepad: the null state 8 is centred", saw(EV_ABS, TEST_HAT0X, 0));
    feed(hid, dpad_up, sizeof(dpad_up));
    CHECK("gamepad: a D-pad press gives nothing", nseen == 0);
    drop(hid);

    ndevs = 0;
    hid = connect(hat_backwards, sizeof(hat_backwards), "ACME Backwards", &inputs);
    d = &devs[0];
    CHECK("hat 0 to 0xFFFFFFFF: an axis, as Linux's hat_min < hat_max makes it",
          inputs == 1 && bit(d->abs, TEST_HAT0X) && !bit(d->abs, TEST_HAT0Y));
    feed(hid, zero, sizeof(zero));
    feed(hid, three, sizeof(three));
    CHECK("hat 0 to 0xFFFFFFFF: its values are reported as they come, with no division",
          saw(EV_ABS, TEST_HAT0X, 3));
    drop(hid);

    ndevs = 0;
    hid = connect(hat_widest, sizeof(hat_widest), "ACME Widest", &inputs);
    d = &devs[0];
    CHECK("hat INT32_MIN to INT32_MAX: an axis, as Linux's 16-bit hat_min 0 and hat_max -1 make it",
          inputs == 1 && bit(d->abs, TEST_HAT0X) && !bit(d->abs, TEST_HAT0Y));
    feed(hid, zero, sizeof(zero));
    feed(hid, three, sizeof(three));
    CHECK("hat INT32_MIN to INT32_MAX: its values are reported with no division",
          saw(EV_ABS, TEST_HAT0X, 3));
    drop(hid);

    ndevs = 0;
    hid = connect(controls, sizeof(controls), "ACME Controls", &inputs);
    d = &devs[0];
    CHECK(
        "device controls: usage 0x21 is ABS_MISC and Battery Strength nothing, as Linux maps them",
        inputs == 1 && bit(d->abs, TEST_MISC) && count_bits(d->abs, 8) == 1);
    drop(hid);
}

/** Read two packets per frame and follow contacts by HID identifier. */
static void rows_multitouch(void)
{
    static const uint8_t touchscreen[] = {
        0x05, 0x0d, 0x09, 0x04, 0xa1, 0x01, /* Digitizer, Touch Screen, Application */
        0x09, 0x22, 0xa1, 0x02,             /* Finger, Logical Collection */
        0x09, 0x42, 0x15, 0,    0x25, 1,    0x75, 8,    0x95, 1,    0x81, 2, /* Tip Switch */
        0x09, 0x51, 0x26, 0xff, 0,    0x81, 2, /* Contact Identifier */
        0x05, 1,    0x09, 0x30, 0x09, 0x31, 0x26, 0xff, 0x7f, 0x75, 16,   0x95,
        2,    0x81, 2,    0xc0,                                              /* X, Y */
        0x05, 0x0d, 0x09, 0x54, 0x25, 8,    0x75, 8,    0x95, 1,    0x81, 2, /* Contact Count */
        0x09, 0x55, 0xb1, 2,    0xc0 /* Contact Count Maximum */
    };
    static const uint8_t first[] = {1, 17, 100, 0, 120, 0, 2};
    static const uint8_t second[] = {1, 42, 180, 0, 220, 0, 0};
    static const uint8_t moved_second[] = {1, 42, 200, 0, 220, 0, 2};
    static const uint8_t moved_first[] = {1, 17, 110, 0, 120, 0, 0};
    static const uint8_t updated[] = {1, 42, 210, 0, 220, 0, 1};
    static const uint8_t lift_second[] = {0, 42, 0, 0, 0, 0, 1};
    static const uint8_t lift_first[] = {0, 17, 0, 0, 0, 0, 1};
    struct hid_dev *hid;
    struct evdev *d;
    int inputs;

    ndevs = 0;
    hid = connect(touchscreen, sizeof(touchscreen), "HID touch screen", &inputs);
    d = &devs[0];
    CHECK("descriptor makes a direct device with eight slots and per-contact axes",
          inputs == 1 && d->props == (1u << INPUT_PROP_DIRECT) && d->slots == 8 &&
              bit(d->abs, ABS_MT_POSITION_X) && bit(d->abs, ABS_MT_POSITION_Y));
    feed(hid, first, sizeof(first));
    CHECK("a first contact waits for the rest of its frame",
          d->id[0] == 0 && !saw(EV_SYN, SYN_REPORT, 0));
    feed(hid, second, sizeof(second));
    CHECK("the second report completes the frame",
          d->id[1] == 1 && saw(EV_ABS, ABS_MT_POSITION_X, 180) && saw(EV_SYN, SYN_REPORT, 0));
    feed(hid, moved_second, sizeof(moved_second));
    CHECK("a reordered contact keeps its slot", d->slot == 1 && d->id[1] == 1);
    feed(hid, moved_first, sizeof(moved_first));
    CHECK("both tracking IDs survive the move",
          d->slot == 0 && d->id[0] == 0 && d->id[1] == 1 && saw(EV_SYN, SYN_REPORT, 0));
    feed(hid, updated, sizeof(updated));
    CHECK("a counted update for only one contact keeps the other active",
          d->id[0] == 0 && d->id[1] == 1 && saw(EV_ABS, ABS_MT_POSITION_X, 210));
    feed(hid, lift_second, sizeof(lift_second));
    CHECK("the second contact is explicitly lifted",
          d->id[0] == 0 && d->id[1] == -1 && saw(EV_ABS, ABS_MT_TRACKING_ID, -1));
    feed(hid, lift_first, sizeof(lift_first));
    CHECK("the last explicit lift clears BTN_TOUCH", d->id[0] == -1 && saw(EV_KEY, BTN_TOUCH, 0));
    drop(hid);
}

/* One contact with two declared Contact ID usages and one data element. */
#define TEST_CONTACT                                                                              \
    0x05, 0x0d, 0x09, 0x42, 0x15, 0, 0x25, 1, 0x75, 8, 0x95, 1, 0x81, 2, 0x05, 1, 0x09, 0x30,     \
        0x09, 0x31, 0x26, 0xff, 0x7f, 0x75, 16, 0x95, 2, 0x81, 2, 0x05, 0x0d, 0x09, 0x47, 0x09,   \
        0x32, 0x25, 1, 0x75, 8, 0x95, 2, 0x81, 2, 0x09, 0x51, 0x09, 0x51, 0x26, 0xff, 0, 0x95, 1, \
        0x81, 2

/* A precision touchpad with two records in one Finger collection in
 * report 1, and one record and no buttons in report 2. */
static const uint8_t precision_pad[] = {
    0x05,
    0x0d,
    0x09,
    5,
    0xa1,
    1,
    0x85,
    1,
    0x09,
    0x22,
    0xa1,
    2,
    TEST_CONTACT,
    TEST_CONTACT,
    0xc0,
    0x05,
    9,
    0x09,
    1,
    0x09,
    2,
    0x25,
    1,
    0x95,
    1,
    0x81,
    2, /* Button 1, with an unused usage for Button 2 */
    0x09,
    2,
    0x09,
    3,
    0x95,
    2,
    0x81,
    2, /* buttons */
    0x05,
    0x0d,
    0x09,
    0x54,
    0x25,
    16,
    0x95,
    1,
    0x81,
    2, /* Contact Count */
    0x85,
    2,
    0x09,
    0x22,
    0xa1,
    2,
    TEST_CONTACT,
    0xc0,
    0x09,
    0x54,
    0x25,
    16,
    0x95,
    1,
    0x81,
    2, /* Contact Count */
    0x85,
    3,
    0x09,
    0x55,
    0x27,
    0xff,
    0xff,
    0,
    0,
    0x75,
    16,
    0xb1,
    2, /* Contact Count Maximum */
    0x09,
    0x52,
    0x25,
    3,
    0x75,
    8,
    0xb1,
    2, /* Input Mode */
    0x06,
    0,
    0xff,
    0x09,
    0xc5,
    0x26,
    0xff,
    0,
    0xb1,
    2,
    0xc0 /* Microsoft profile */
};

/* The Feature reply, with its report ID, maximum, mode and vendor byte. */
static const uint8_t pad_feature[] = {3, 16, 0, 0, 0x83};

/** Open the model feature exchange with its numbered GET_REPORT. */
static struct hid_dev *mt_feature_open(void)
{
    struct hid_dev *hid;
    int value = 0, write = 0;
    uint8_t data[sizeof(pad_feature)];

    ndevs = 0;
    if (parse(precision_pad, sizeof(precision_pad), &hid) != 0) {
        CHECK("the precision touchpad descriptor parses", 0);
        return NULL;
    }
    CHECK("the interface is ready for GET_REPORT before registration",
          hidinput_mt_features(hid) == 1 &&
              hidinput_mt_feature_request(hid, data, sizeof(data), &value, &write) ==
                  sizeof(data) &&
              value == 0x303 && !write && ndevs == 0);
    return hid;
}

/** Read the feature before publishing slots and preserve its other bytes. */
static void mt_feature_rows(void)
{
    struct hid_dev *hid = mt_feature_open();
    struct evdev *d = &devs[0];
    int value = 0, write = 0;
    uint8_t data[sizeof(pad_feature)];

    if (!hid) {
        return;
    }
    hidinput_mt_feature_answer(hid, pad_feature, sizeof(pad_feature));
    CHECK("SET_REPORT changes only Input Mode to the touchpad value",
          hidinput_mt_feature_request(hid, data, sizeof(data), &value, &write) == sizeof(data) &&
              value == 0x303 && write && data[0] == 3 && data[1] == 16 && data[2] == 0 &&
              data[3] == 3 && data[4] == 0x83);
    hidinput_mt_feature_answer(hid, NULL, sizeof(data));
    CHECK("finish with the reported slot count", hidinput_connect(hid) == 1 && d->slots == 16);
    drop(hid);
}

/** A refused mode cannot publish a node for contact reports. */
static void mt_failed_mode_rows(void)
{
    static const uint8_t ready[] = {3, 16, 0, 3};
    struct hid_dev *hid = mt_feature_open();
    int value = 0, write = 0;
    uint8_t data[sizeof(pad_feature)];

    if (!hid) {
        return;
    }
    hidinput_mt_feature_answer(hid, pad_feature, sizeof(pad_feature) - 1);
    CHECK("short GET leaves the shared feature unmodified and the node unpublished",
          hidinput_mt_feature_request(hid, data, sizeof(data), &value, &write) == 0 &&
              hidinput_connect(hid) < 0 && ndevs == 0);
    drop(hid);

    hid = mt_feature_open();
    if (!hid) {
        return;
    }
    hidinput_mt_feature_answer(hid, pad_feature, sizeof(pad_feature));
    CHECK("a successful GET can be followed by an attempted SET",
          hidinput_mt_feature_request(hid, data, sizeof(data), &value, &write) == sizeof(data) &&
              write);
    hidinput_mt_feature_answer(hid, NULL, -1);
    CHECK("refused SET leaves the touch node unpublished", hidinput_connect(hid) < 0 && ndevs == 0);
    drop(hid);

    hid = mt_feature_open();
    if (!hid) {
        return;
    }
    hidinput_mt_feature_answer(hid, ready, sizeof(ready));
    CHECK("a short GET with the required mode publishes the reported slots",
          hidinput_mt_feature_request(hid, data, sizeof(data), &value, &write) == 0 &&
              hidinput_connect(hid) == 1 && devs[0].slots == 16);
    drop(hid);
}

/* A keyboard and Precision Touchpad sharing an input report ID. */
static const uint8_t mixed_pad[] = {0x05, 1,
                                    0x09, 6,
                                    0xa1, 1,
                                    0x85, 1,
                                    0x05, 7,
                                    0x09, 4,
                                    0x15, 0,
                                    0x25, 1,
                                    0x75, 8,
                                    0x95, 1,
                                    0x81, 2,
                                    0xc0, 0x05,
                                    0x0d, 0x09,
                                    5,    0xa1,
                                    1,    0x09,
                                    0x22, 0xa1,
                                    2,    TEST_CONTACT,
                                    0xc0, 0x05,
                                    9,    0x09,
                                    2,    0x25,
                                    1,    0x95,
                                    1,    0x81,
                                    2,    0x85,
                                    4,    0x09,
                                    3,    0x81,
                                    2,    0x05,
                                    0x0d, 0x85,
                                    3,    0x09,
                                    0x52, 0x25,
                                    3,    0xb1,
                                    2,    0x06,
                                    0,    0xff,
                                    0x09, 0xc5,
                                    0x26, 0xff,
                                    0,    0xb1,
                                    2,    0xc0};

/** A failed mode leaves the shared report's keyboard available. */
static void mt_mixed_rows(void)
{
    static const uint8_t first[] = {1, 1, 1, 100, 0, 120, 0, 1, 1, 17, 1};
    struct hid_dev *hid;
    struct evdev_keymap_entry ke;
    int inputs, value = 0, write = 0;
    uint8_t feature[3];
    uint16_t old;

    ndevs = 0;
    hid = connect(mixed_pad, sizeof(mixed_pad), "Keyboard with touchpad", &inputs);
    CHECK("shared report maps into two applications",
          inputs == 2 && devs[0].keyboard && devs[1].slots > 0 && bit(devs[1].keys, BTN_LEFT));
    k_memset(&ke, 0, sizeof(ke));
    ke.scancode = 0x70004;
    ke.keycode = KEY_B;
    CHECK("the touchpad cannot remap the keyboard key",
          devs[1].km.set && devs[1].km.set(devs[1].km.ctx, &ke, &old) == -EINVAL &&
              !devs[1].km.maps(devs[1].km.ctx, KEY_A));
    feed(hid, first, sizeof(first));
    CHECK("shared report gives the keyboard key and touchpad click",
          bit(devs[0].down, KEY_A) && bit(devs[1].down, BTN_LEFT) && devs[1].id[0] == 0);
    drop(hid);

    ndevs = 0;
    if (parse(mixed_pad, sizeof(mixed_pad), &hid) != 0) {
        CHECK("the mixed descriptor parses", 0);
        return;
    }
    CHECK("the shared mode feature is read before publishing",
          hidinput_mt_features(hid) == 1 &&
              hidinput_mt_feature_request(hid, feature, sizeof(feature), &value, &write) ==
                  sizeof(feature) &&
              value == 0x303 && !write);
    hidinput_mt_feature_answer(hid, NULL, -1);
    CHECK("failed mode leaves only the keyboard, even with a separate button report",
          hidinput_connect(hid) == 1 && ndevs == 1 && devs[0].keyboard);
    feed(hid, first, sizeof(first));
    CHECK("the shared report still sends the key", bit(devs[0].down, KEY_A));
    drop(hid);
}

/** Process repeated records, palm and hover, and first-packet buttons. */
static void mt_precision_rows(void)
{
    static const uint8_t first[] = {1, 1,   100, 0, 120, 0,  0, 1, 17, 0, 180,
                                    0, 220, 0,   1, 1,   42, 0, 1, 0,  3};
    static const uint8_t next[] = {2, 1, 75, 0, 90, 0, 1, 1, 51, 0};
    static const uint8_t buttonless[] = {2, 1, 80, 0, 90, 0, 1, 1, 51, 1};
    static const uint8_t lifts[] = {1, 0, 0, 0, 0, 0,  1, 0, 42, 0, 0,
                                    0, 0, 0, 1, 0, 17, 0, 0, 1,  2};
    static const uint8_t hover[] = {2, 0, 80, 0, 90, 0, 1, 1, 51, 1};
    struct hid_dev *hid;
    struct evdev *d = &devs[0];
    struct evdev_keymap_entry ke;
    uint16_t old;
    int inputs;

    ndevs = 0;
    hid = connect(precision_pad, sizeof(precision_pad), "Precision Touchpad", &inputs);
    CHECK("the precision touchpad has the mapped buttons", inputs == 1 && bit(d->keys, BTN_LEFT) &&
                                                               bit(d->keys, BTN_RIGHT) &&
                                                               !bit(d->keys, BTN_MIDDLE));
    k_memset(&ke, 0, sizeof(ke));
    ke.scancode = 0x00090002u;
    ke.keycode = 30;
    CHECK("the frame's button mapping stays fixed", d->km.set(d->km.ctx, &ke, &old) == -EINVAL);
    feed(hid, first, sizeof(first));
    CHECK("both records are read before the continuation",
          d->id[0] == 0 && d->id[1] == 1 && !saw(EV_SYN, SYN_REPORT, 0) &&
              saw(EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_PALM) && saw(EV_ABS, ABS_MT_DISTANCE, 1));
    feed(hid, next, sizeof(next));
    CHECK("the continuation completes the frame and keeps the first packet's left button",
          d->id[2] == 2 && saw(EV_SYN, SYN_REPORT, 0) && saw(EV_KEY, BTN_LEFT, 1));
    feed(hid, buttonless, sizeof(buttonless));
    CHECK("a new frame with no button fields keeps the held left button",
          bit(d->down, BTN_LEFT) && !saw(EV_KEY, BTN_LEFT, 0));
    feed(hid, lifts, sizeof(lifts));
    CHECK("the two lift records carry their separate slot selectors",
          is(0, EV_ABS, ABS_MT_SLOT, 1) && is(1, EV_ABS, ABS_MT_TRACKING_ID, -1) &&
              is(2, EV_ABS, ABS_MT_SLOT, 0) && is(3, EV_ABS, ABS_MT_TRACKING_ID, -1) &&
              saw(EV_KEY, BTN_RIGHT, 1) && d->id[2] == 2);
    feed(hid, hover, sizeof(hover));
    CHECK("hover keeps the slot and pointer BTN_TOUCH as Linux does",
          d->id[2] == 2 && saw(EV_ABS, ABS_MT_DISTANCE, 1) && bit(d->down, BTN_TOUCH));
    drop(hid);
}

/** An active-contact report lifts a slot omitted by the device. */
static void mt_implicit_lift_rows(void)
{
    static const uint8_t two[] = {1, 1,   100, 0, 120, 0,  1, 1, 17, 1, 180,
                                  0, 220, 0,   1, 1,   42, 0, 0, 0,  2};
    static const uint8_t one[] = {2, 1, 180, 0, 220, 0, 1, 1, 42, 1};
    struct hid_dev *hid;
    struct evdev *d = &devs[0];

    ndevs = 0;
    if (parse(precision_pad, sizeof(precision_pad), &hid) != 0) {
        CHECK("the model contact descriptor parses", 0);
        return;
    }
    hid->vendor = 0x1aad;
    hid->product = 0x000f;
    CHECK("the GoodTouch profile publishes its input device", hidinput_connect(hid) == 1);
    feed(hid, two, sizeof(two));
    CHECK("two contacts start", d->id[0] == 0 && d->id[1] == 1);
    feed(hid, one, sizeof(one));
    CHECK("the contact absent from the frame is lifted by the profile",
          d->id[0] == -1 && d->id[1] == 1 && saw(EV_ABS, ABS_MT_SLOT, 0) &&
              saw(EV_ABS, ABS_MT_TRACKING_ID, -1));
    drop(hid);
}

/** Run the rows. */
int main(void)
{
    rows_parse_tablet();
    rows_parse_keyboards();
    rows_parse_media();
    rows_parse_malformed();
    rows_parse_fields();
    rows_map_tablet();
    rows_map_mouse();
    rows_map_keyboard();
    rows_map_media();
    rows_map_apps();
    rows_multiplier();
    rows_gamepad();
    rows_multitouch();
    mt_feature_rows();
    mt_failed_mode_rows();
    mt_mixed_rows();
    mt_precision_rows();
    mt_implicit_lift_rows();
    if (fails) {
        printf("hid_parse_hosttest: %d failed\n", fails);
        return 1;
    }
    printf("hid_parse_hosttest: ok\n");
    return 0;
}
