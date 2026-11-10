/*
 * hid.c: the USB HID class driver, Linux's usbhid with hid-generic, and its
 * boot-protocol drivers, usbkbd and usbmouse, as the fallback.
 *
 * A HID interface describes its own reports: its report descriptor says
 * which bits of which report are which key, button or axis. The core reads
 * the descriptor of each HID interface of a device when it configures the
 * device, and this driver parses it
 * (`hid_parse.c`), maps its usages to event devices and reads each report
 * through it (`hid_input.c`), as Linux does. A keyboard's or mouse's
 * reports are then whatever its descriptor says, and a device that has no
 * boot protocol, such as QEMU's tablet, works as well as one that has. Each
 * interface is driven on its own, as Linux binds usbhid to each, so a
 * keyboard whose media keys are a second interface gives them too.
 *
 * A keyboard or mouse whose descriptor cannot be read or parsed is put in
 * the boot protocol, whose two report layouts are fixed. There a HID report
 * is a state, these keys are down now, and a key event is an event, this
 * key went down; so each boot report is compared with the one before it,
 * and the difference is what the input layer is given, as Linux's key
 * codes, the ones the PS/2 keyboard's set-1 codes name.
 *
 * Either way a keyboard's keys go through the input layer, which repeats a
 * held key in software as it does a PS/2 one's and gives the keys to the
 * console; and the other way, a keyboard's LEDs are an output report the
 * core sends when it differs from what the keyboard was last sent.
 */

#include "hid.h"

#include "usb.h"

#include "../evdev.h"
#include "../input.h"
#include "../kbd.h"
#include "../kd.h"
#include "../klog.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../mouse.h"
#include "core.h"
#include "hid_input.h"
#include "hid_parse.h"

/* A boot keyboard's report: one byte of modifiers, one reserved, and up to six
 * usages of keys that are down. A boot mouse's: one byte of buttons and two
 * signed movements, with a wheel byte on devices that have one. */
#define KEYBOARD_REPORT 8
#define MOUSE_REPORT    3
#define MOUSE_WHEELED   4
#define KEYS_IN_REPORT  6

/* A boot keyboard's output report: one byte, an LED a bit. */
#define REPORT_NUM_LOCK    0x01
#define REPORT_CAPS_LOCK   0x02
#define REPORT_SCROLL_LOCK 0x04

/* The wValue of SET_REPORT for the boot keyboard's output report: type 2,
 * Output, and ID 0. */
#define BOOT_OUTPUT 0x0200

/* A device in the report protocol, `hid_state.mode`, beside HID_KEYBOARD and
 * HID_MOUSE. */
#define HID_REPORT 3

/* The bytes of a boot keyboard's output report, its LEDs. */
#define BOOT_OUTPUT_LEN 1

/* Which report `hid_output_report` gave last. */
#define OFFER_NONE       0
#define OFFER_LEDS       1
#define OFFER_MULTIPLIER 2

/*
 * The usages this kernel has a scancode for, from 0 to USAGES - 1, whose key
 * code is `set1_keycode`'s for that scancode. A value of zero is a key it
 * does not name; a value with 0xE000 set is one whose scancode is the
 * two-byte extended form, which is how the arrows and the keypad's twins are
 * told apart from the keys they share a code with. Print Screen is E0 37 and
 * Pause the extended 0x45, as `set1_keycode` names them. The last three are
 * Mute, Volume Up and Volume Down, as a PS/2 keyboard sends them.
 */
#define EXT    0xE000u
#define USAGES 0x82u

/* The keyboard page's usages, the high half of an MSC_SCAN. */
#define KEYBOARD_PAGE 0x70000u

static const uint16_t usage_to_scancode[USAGES] = {
    0,          0,          0,          0,          0x1E,       0x30,       0x2E,       0x20,
    0x12,       0x21,       0x22,       0x23,       0x17,       0x24,       0x25,       0x26,
    0x32,       0x31,       0x18,       0x19,       0x10,       0x13,       0x1F,       0x14,
    0x16,       0x2F,       0x11,       0x2D,       0x15,       0x2C,       0x02,       0x03,
    0x04,       0x05,       0x06,       0x07,       0x08,       0x09,       0x0A,       0x0B,
    0x1C,       0x01,       0x0E,       0x0F,       0x39,       0x0C,       0x0D,       0x1A,
    0x1B,       0x2B,       0x2B,       0x27,       0x28,       0x29,       0x33,       0x34,
    0x35,       0x3A,       0x3B,       0x3C,       0x3D,       0x3E,       0x3F,       0x40,
    0x41,       0x42,       0x43,       0x44,       0x57,       0x58,       EXT | 0x37, 0x46,
    EXT | 0x45, EXT | 0x52, EXT | 0x47, EXT | 0x49, EXT | 0x53, EXT | 0x4F, EXT | 0x51, EXT | 0x4D,
    EXT | 0x4B, EXT | 0x50, EXT | 0x48, 0x45,       EXT | 0x35, 0x37,       0x4A,       0x4E,
    EXT | 0x1C, 0x4F,       0x50,       0x51,       0x4B,       0x4C,       0x4D,       0x47,
    0x48,       0x49,       0x52,       0x53,       0x56,       EXT | 0x5D, 0,          0,
    0,          0,          0,          0,          0,          0,          0,          0,
    0,          0,          0,          0,          0,          0,          0,          0,
    0,          0,          0,          0,          0,          0,          0,          EXT | 0x20,
    EXT | 0x30, EXT | 0x2E};

/* The modifier byte's bits, in order, and the scancode each sends. Right Ctrl
 * and right Alt are the extended form of the left-hand codes; right Shift and
 * each GUI key have codes of their own. */
static const uint16_t modifier_scancode[8] = {0x1D,       0x2A, 0x38,       EXT | 0x5B,
                                              EXT | 0x1D, 0x36, EXT | 0x38, EXT | 0x5C};

/* What an interface was reporting last time, and what it was sent. */
struct hid_state {
    int mode; /* HID_KEYBOARD or HID_MOUSE, or HID_REPORT */
    uint8_t modifiers;
    uint8_t keys[KEYS_IN_REPORT];
    struct input_keyboard kb; /* a boot keyboard's keys in the input layer */
    struct evdev *mouse;      /* a boot mouse's event device, or NULL */
    struct hid_dev *hid;      /* a report-protocol device's tables, or NULL */
    char name[HID_NAME_MAX];  /* a boot device's name */
    char phys[24];            /* its EVIOCGPHYS, "usb-N/inputI" */
    int offer;                /* OFFER_*: what `hid_output_report` gave last */
    int multiplier;           /* a multiplier report is still to be sent */
    int leds_failed;          /* an LED report failed since it was taken */
    int shown_len;            /* the LED report last taken, -1 before one */
    /* The LED report last taken and the one offered, each of `out_cap`
     * bytes: a report-protocol device's in its tables (`hid_dev.outbuf`),
     * a boot keyboard's in `boot_out`. */
    uint8_t *shown;
    uint8_t *offered;
    int out_cap;
    int offered_len;
    uint8_t boot_out[2][BOOT_OUTPUT_LEN];
};

/* The state of each interface taken, from the heap, by the core's device
 * table entry plus one and the interface's place on its device; NULL for
 * one not taken. */
static struct hid_state *states[USB_MAX_DEVICES + 1][USB_HID_INTERFACES];

/** Append the text `text` to the string in `out`, of `size` bytes. */
static void append(char *out, size_t size, const char *text)
{
    size_t i = 0;

    while (out[i]) {
        i++;
    }
    while (*text && i + 1 < size) {
        out[i++] = *text++;
    }
    out[i] = 0;
}

/** Append `n`, from 0 to 99, in decimal to the string in `out`. */
static void append_number(char *out, size_t size, int n)
{
    char digits[3];

    digits[0] = (char)(n >= 10 ? '0' + n / 10 % 10 : '0' + n);
    digits[1] = (char)(n >= 10 ? '0' + n % 10 : 0);
    digits[2] = 0;
    append(out, size, digits);
}

/** Append `n` to the string in `out` as four hexadecimal digits, as Linux
 * prints a vendor or a product. */
static void append_hex4(char *out, size_t size, unsigned n)
{
    static const char hex[] = "0123456789abcdef";
    char digits[5];
    int i;

    for (i = 0; i < 4; i++) {
        digits[i] = hex[(n >> (12 - 4 * i)) & 0xFu];
    }
    digits[4] = 0;
    append(out, size, digits);
}

/** Write the EVIOCGPHYS of interface number `interface` of `device`,
 * "usb-N/inputI", into `state`, as usbhid ends it with the interface's
 * bInterfaceNumber. */
static void name_phys(struct hid_state *state, int device, int interface)
{
    char digits[4];
    int n = interface & 0xFF, i = 0;

    if (n >= 100) {
        digits[i++] = (char)('0' + n / 100);
    }
    if (n >= 10) {
        digits[i++] = (char)('0' + n / 10 % 10);
    }
    digits[i++] = (char)('0' + n % 10);
    digits[i] = 0;
    state->phys[0] = 0;
    append(state->phys, sizeof(state->phys), "usb-");
    append_number(state->phys, sizeof(state->phys), device);
    append(state->phys, sizeof(state->phys), "/input");
    append(state->phys, sizeof(state->phys), digits);
}

/**
 * Write a device's name into `out`, `size` bytes, as usbhid, usbkbd and
 * usbmouse write it: the manufacturer and the product, a space between,
 * or when the device has neither string, `fallback` and its vendor and
 * product in hex.
 */
static void name_device(char *out, size_t size, const struct hid_ids *ids, const char *fallback)
{
    out[0] = 0;
    if (ids->manufacturer && ids->manufacturer[0]) {
        append(out, size, ids->manufacturer);
    }
    if (ids->product_name && ids->product_name[0]) {
        if (out[0]) {
            append(out, size, " ");
        }
        append(out, size, ids->product_name);
    }
    if (!out[0]) {
        append(out, size, fallback);
        append_hex4(out, size, ids->vendor);
        append(out, size, ":");
        append_hex4(out, size, ids->product);
    }
}

/**
 * Copy the serial string `serial`, UTF-8 or NULL, into `out` of `size`
 * bytes, a character that would not fit skipped and the copy going on, as
 * usbhid's `usb_string` into `hid->uniq` gets it from utf16s_to_utf8s;
 * "" for none.
 */
static void copy_serial(char *out, size_t size, const char *serial)
{
    size_t i = 0, o = 0, need;

    while (serial && serial[i]) {
        uint8_t c = (uint8_t)serial[i];

        need = c < 0xC0 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        if (o + need >= size) {
            for (; need > 0 && serial[i]; need--) {
                i++;
            }
            continue;
        }
        for (; need > 0 && serial[i]; need--) {
            out[o++] = serial[i++];
        }
    }
    out[o] = 0;
}

/** Whether `device` and `iface` name an interface the table has room for. */
static int in_range(int device, int iface)
{
    return device > 0 && device <= USB_MAX_DEVICES && iface >= 0 && iface < USB_HID_INTERFACES;
}

/** The state of interface `iface` of `device`, or NULL for one not taken
 * or out of range. */
static struct hid_state *state_of(int device, int iface)
{
    return in_range(device, iface) ? states[device][iface] : 0;
}

/** A new state, zeroed, for interface `iface` of `device`, which is in
 * range and has none; NULL when the heap has none. */
static struct hid_state *state_new(int device, int iface)
{
    struct hid_state *state = (struct hid_state *)kzalloc(sizeof(*state));

    states[device][iface] = state;
    return state;
}

/** Give back the state of interface `iface` of `device`, in range. */
static void state_free(int device, int iface)
{
    kfree(states[device][iface]);
    states[device][iface] = 0;
}

/** The scancode of `usage`, 0 for one the table does not name. */
static uint16_t scancode_of(uint8_t usage)
{
    return usage < USAGES ? usage_to_scancode[usage] : 0;
}

/**
 * Give the input layer the key of `usage`, pressed or `released`: the key
 * code `set1_keycode` gives for its scancode `code`, 0 for a usage the table
 * does not name, with the usage in the keyboard page as its MSC_SCAN. A named
 * key then takes the third of the four stamps the latency record prints.
 */
static void send_key(struct hid_state *state, uint8_t usage, uint16_t code, int released)
{
    unsigned scan = (code & 0x7Fu) | ((code & EXT) ? 0x80u : 0u);
    unsigned key = (code & 0xFFu) ? (unsigned)set1_keycode(scan) : 0;

    input_key_event(&state->kb, KEYBOARD_PAGE | usage, key, !released);
    if (key != 0) {
        usb_stamp_wake();
    }
}

/** Whether `usage` is in a report's six key slots. */
static int held(const uint8_t *keys, uint8_t usage)
{
    int i;

    for (i = 0; i < KEYS_IN_REPORT; i++) {
        if (keys[i] == usage) {
            return 1;
        }
    }
    return 0;
}

/**
 * The keyboard half: the difference between two reports.
 *
 * A modifier bit or a key usage in the new report and not the old went down;
 * one in the old and not the new came up; one in both is held and sends
 * nothing, which is why the previous report is kept. A report of zeros says
 * nothing is down, and its releases are the difference like any other.
 *
 * A key slot with an error usage, 0x01 to 0x03, is the keyboard saying it
 * cannot tell which keys are down: too many at once, or a fault in its own
 * scanning. The modifier byte is still read, but the key slots are ignored
 * and the previous keys kept, since taking them would release every key that
 * is held and press it again when the keyboard recovers.
 */
static void keyboard_report(struct hid_state *state, const uint8_t *report)
{
    uint8_t modifiers = report[0];
    const uint8_t *keys = &report[2];
    int i;

    for (i = 0; i < 8; i++) {
        uint8_t bit = (uint8_t)(1u << i);

        if ((modifiers & bit) && !(state->modifiers & bit)) {
            send_key(state, (uint8_t)(0xE0 + i), modifier_scancode[i], 0);
        } else if (!(modifiers & bit) && (state->modifiers & bit)) {
            send_key(state, (uint8_t)(0xE0 + i), modifier_scancode[i], 1);
        }
    }
    state->modifiers = modifiers;

    for (i = 0; i < KEYS_IN_REPORT; i++) {
        if (keys[i] >= 0x01u && keys[i] <= 0x03u) {
            return;
        }
    }

    for (i = 0; i < KEYS_IN_REPORT; i++) {
        uint8_t usage = keys[i];

        if (usage > 0x03u && !held(state->keys, usage)) {
            send_key(state, usage, scancode_of(usage), 0);
        }
    }
    for (i = 0; i < KEYS_IN_REPORT; i++) {
        uint8_t usage = state->keys[i];

        if (usage > 0x03u && !held(keys, usage)) {
            send_key(state, usage, scancode_of(usage), 1);
        }
    }

    for (i = 0; i < KEYS_IN_REPORT; i++) {
        state->keys[i] = keys[i];
    }
}

/** Free the tables of `hid`, NULL accepted. */
static void free_tables(struct hid_dev *hid)
{
    if (!hid) {
        return;
    }
    kfree(hid->desc.fields);
    kfree(hid->desc.usages);
    kfree(hid->desc.collections);
    kfree(hid->inbuf);
    kfree(hid->outbuf);
    kfree(hid);
}

/**
 * Parse `rdesc` into a new `struct hid_dev` with tables of the sizes the
 * descriptor needs, counted by a first pass. Returns 0 with `*out` set, or
 * the parser's error, or -ENOMEM.
 */
static int parse_tables(const uint8_t *rdesc, int length, struct hid_dev **out)
{
    struct hid_dev *hid = (struct hid_dev *)kzalloc(sizeof(*hid));
    uint32_t i, insize = 0;
    int err;

    *out = 0;
    if (!hid) {
        return -ENOMEM;
    }
    err = hid_parse(&hid->desc, rdesc, length);
    if (err) {
        kfree(hid);
        return err;
    }
    /* One spare entry each, so that an empty table is still a table. */
    hid->desc.fields =
        (struct hid_field *)kmalloc((hid->desc.max_fields + 1) * sizeof(struct hid_field));
    hid->desc.usages =
        (struct hid_usage *)kmalloc((hid->desc.max_usages + 1) * sizeof(struct hid_usage));
    hid->desc.collections = (struct hid_collection *)kmalloc((hid->desc.max_collections + 1) *
                                                             sizeof(struct hid_collection));
    if (!hid->desc.fields || !hid->desc.usages || !hid->desc.collections) {
        free_tables(hid);
        return -ENOMEM;
    }
    err = hid_parse(&hid->desc, rdesc, length);
    if (err) {
        free_tables(hid);
        return err;
    }
    for (i = 0; i < hid->desc.nreports; i++) {
        const struct hid_report *r = &hid->desc.reports[i];

        if (r->type == HID_INPUT_REPORT && hid_report_len(r) > insize) {
            insize = hid_report_len(r);
        }
    }
    hid->insize = insize;
    hid->inbuf = (uint8_t *)kzalloc(insize + 1);
    for (i = 0; i < hid->desc.nreports; i++) {
        const struct hid_report *r = &hid->desc.reports[i];

        if (r->type != HID_INPUT_REPORT && hid_report_len(r) > hid->outsize) {
            hid->outsize = hid_report_len(r);
        }
    }
    hid->outbuf = (uint8_t *)kzalloc(2 * hid->outsize + 1);
    if (!hid->inbuf || !hid->outbuf) {
        free_tables(hid);
        return -ENOMEM;
    }
    *out = hid;
    return 0;
}

int hid_attach(int device, int iface, const struct hid_ids *ids, const uint8_t *rdesc, int length)
{
    struct hid_state *state;
    struct hid_dev *hid;
    int err;

    if (!in_range(device, iface)) {
        return -ENODEV;
    }
    if (states[device][iface]) {
        return 0;
    }
    err = parse_tables(rdesc, length, &hid);
    if (err) {
        klog(KLOG_WARN, "usb", "hid: device %d interface %d: %x:%x: report descriptor refused: %s",
             device, ids->interface, (int)ids->vendor, (int)ids->product,
             err == -ENOMEM  ? "no memory for its tables"
             : err == -E2BIG ? "past this kernel's tables"
                             : "malformed");
        return err;
    }
    state = state_new(device, iface);
    if (!state) {
        klog(KLOG_WARN, "usb", "hid: device %d interface %d: no memory for its state", device,
             ids->interface);
        free_tables(hid);
        return -ENOMEM;
    }
    name_device(hid->name, sizeof(hid->name), ids, "HID ");
    name_phys(state, device, ids->interface);
    hid->phys = state->phys;
    copy_serial(hid->uniq, sizeof(hid->uniq), ids->serial);
    hid->vendor = ids->vendor;
    hid->product = ids->product;
    hid->version = ids->version;
    if (hidinput_connect(hid) < 0) {
        klog(KLOG_INFO, "usb", "hid: device %d interface %d: %s: no input device in its descriptor",
             device, ids->interface, hid->name);
        free_tables(hid);
        state_free(device, iface);
        return -ENODEV;
    }
    state->hid = hid;
    state->mode = HID_REPORT;
    state->offer = OFFER_NONE;
    state->multiplier = 1;
    state->leds_failed = 0;
    state->shown_len = -1;
    state->offered = hid->outbuf;
    state->shown = hid->outbuf + hid->outsize;
    state->out_cap = (int)hid->outsize;
    klog(KLOG_INFO, "usb", "hid: device %d interface %d: %s on %s, input devices: %d", device,
         ids->interface, hid->name, state->phys, (int)hid->ninputs);
    return 0;
}

int hid_attach_boot(int device, int iface, int kind, const struct hid_ids *ids)
{
    struct hid_state *state;

    if (!in_range(device, iface) || (kind != HID_KEYBOARD && kind != HID_MOUSE)) {
        return -ENODEV;
    }
    if (states[device][iface]) {
        return 0;
    }
    state = state_new(device, iface);
    if (!state) {
        klog(KLOG_WARN, "usb", "hid: device %d interface %d: no memory for its state", device,
             ids->interface);
        return -ENOMEM;
    }
    state->mode = kind;
    state->offer = OFFER_NONE;
    state->multiplier = 0;
    state->leds_failed = 0;
    state->shown_len = -1;
    state->offered = state->boot_out[0];
    state->shown = state->boot_out[1];
    state->out_cap = BOOT_OUTPUT_LEN;
    name_phys(state, device, ids->interface);
    if (kind == HID_MOUSE) {
        name_device(state->name, sizeof(state->name), ids, "USB HIDBP Mouse ");
        state->mouse = mouse_register(state->name, state->phys, EVDEV_BUS_USB, ids->vendor,
                                      ids->product, MOUSE_WHEEL | MOUSE_HI_RES | MOUSE_SIDE);
        /* An open of `mouse` fails until a mouse of any kind is found. */
        input_mouse_found();
        return 0;
    }
    name_device(state->name, sizeof(state->name), ids, "USB HIDBP Keyboard ");
    input_keyboard_attach(&state->kb, state->name, state->phys, EVDEV_BUS_USB, ids->vendor,
                          ids->product);
    return 0;
}

int hid_report_size(int device, int iface)
{
    struct hid_state *state = state_of(device, iface);

    return state && state->mode == HID_REPORT ? (int)state->hid->insize : 0;
}

void hid_report(int device, int iface, const uint8_t *report, int length)
{
    struct hid_state *state = state_of(device, iface);

    if (!state || length < 0) {
        return;
    }
    switch (state->mode) {
    case HID_REPORT:
        hid_input_report(state->hid, report, (uint32_t)length);
        return;
    case HID_KEYBOARD:
        if (length >= KEYBOARD_REPORT) {
            keyboard_report(state, report);
        }
        return;
    case HID_MOUSE:
        if (length >= MOUSE_REPORT) {
            /* A report's Y is positive toward the user, as REL_Y is. */
            mouse_report(state->mouse, report[0] & 0x1Fu, (int)(int8_t)report[1],
                         (int)(int8_t)report[2],
                         length >= MOUSE_WHEELED ? (int)(int8_t)report[3] : 0, 0);
        }
        return;
    default:
        return;
    }
}

/** The LED_* bits of the LEDs the input layer wants lit. */
static unsigned int leds_wanted(void)
{
    unsigned int wanted = input_leds();
    unsigned int leds = 0;

    if (wanted & LED_NUM) {
        leds |= 1u << LED_NUML;
    }
    if (wanted & LED_CAP) {
        leds |= 1u << LED_CAPSL;
    }
    if (wanted & LED_SCR) {
        leds |= 1u << LED_SCROLLL;
    }
    return leds;
}

/** Build the LED report of `state` in `out`, `size` bytes, with its wValue
 * in `*value`; its bytes, or -1 when the device has no LEDs. */
static int leds_report(struct hid_state *state, uint8_t *out, int size, int *value)
{
    unsigned int leds = leds_wanted();

    if (state->mode == HID_REPORT) {
        return hidinput_led_report(state->hid, leds, out, size, value);
    }
    if (state->mode != HID_KEYBOARD || size < 1) {
        return -1;
    }
    out[0] = (uint8_t)(((leds >> LED_NUML) & 1u ? REPORT_NUM_LOCK : 0) |
                       ((leds >> LED_CAPSL) & 1u ? REPORT_CAPS_LOCK : 0) |
                       ((leds >> LED_SCROLLL) & 1u ? REPORT_SCROLL_LOCK : 0));
    *value = BOOT_OUTPUT;
    return 1;
}

/** Whether the `n` bytes at `a` and `b` are the same. */
static int same_bytes(const uint8_t *a, const uint8_t *b, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

int hid_output_size(int device, int iface)
{
    struct hid_state *state = state_of(device, iface);

    return state && state->mode ? state->out_cap : 0;
}

int hid_output_report(int device, int iface, uint8_t *out, int size, int *value)
{
    struct hid_state *state = state_of(device, iface);
    int n;

    if (!state || !state->mode) {
        return -1;
    }
    state->offer = OFFER_NONE;
    if (state->multiplier) {
        n = hidinput_multiplier_report(state->hid, out, size, value);
        if (n >= 0) {
            state->offer = OFFER_MULTIPLIER;
            return n;
        }
        state->multiplier = 0;
    }
    if (state->leds_failed) {
        return -1;
    }
    n = leds_report(state, state->offered, state->out_cap, value);
    if (n < 0 || n > size) {
        return -1;
    }
    if (n == state->shown_len && same_bytes(state->offered, state->shown, n)) {
        return -1;
    }
    k_memcpy(out, state->offered, (size_t)n);
    state->offered_len = n;
    state->offer = OFFER_LEDS;
    return n;
}

void hid_output_sent(int device, int iface, int ok)
{
    struct hid_state *state = state_of(device, iface);

    if (!state) {
        return;
    }
    if (state->offer == OFFER_MULTIPLIER) {
        state->multiplier = 0;
        hidinput_multiplier_set(state->hid, ok);
    } else if (state->offer == OFFER_LEDS) {
        if (ok) {
            k_memcpy(state->shown, state->offered, (size_t)state->offered_len);
            state->shown_len = state->offered_len;
        } else {
            if (!state->leds_failed) {
                klog(KLOG_WARN, "usb", "hid: device %d interface %d: the LED report failed", device,
                     iface);
            }
            state->leds_failed = 1;
        }
    }
    state->offer = OFFER_NONE;
}

void hid_gone(int device)
{
    struct hid_state *state;
    int iface;

    for (iface = 0; iface < USB_HID_INTERFACES; iface++) {
        state = state_of(device, iface);
        if (!state) {
            continue;
        }
        if (state->mode == HID_REPORT) {
            hidinput_disconnect(state->hid);
            free_tables(state->hid);
        }
        input_keyboard_detach(&state->kb);
        evdev_unregister(state->mouse);
        state_free(device, iface);
    }
}
