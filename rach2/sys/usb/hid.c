/*
 * hid.c: a USB keyboard and mouse, in the protocol every firmware uses.
 *
 * The boot protocol is two fixed report layouts, and taking it removes the
 * report-descriptor parser, which is the largest part of a HID stack and the
 * part with nothing to teach about USB.
 *
 * The work here is the difference between what USB sends and what this kernel
 * already has. A HID report is a state: these keys are down now. A key event
 * is an event: this key went down, that one came up. So each report is
 * compared with the one before it, and the difference is what the input layer
 * is given, as Linux's key codes, the ones the PS/2 keyboard's set-1 codes
 * name. No code downstream has to tell the two kinds of keyboard apart, and
 * a held key repeats there, in software, as a PS/2 one does.
 *
 * Each keyboard is an event device of its own too, "USB keyboard N", whose
 * keys are reported with the usage as MSC_SCAN, in the keyboard page 7, as
 * Linux's HID driver reports them.
 *
 * The other way, a keyboard's LEDs are its one output report, a byte the
 * core sends when it differs from what the keyboard was last sent.
 */

#include "hid.h"

#include "usb.h"

#include "../evdev.h"
#include "../input.h"
#include "../kbd.h"
#include "../kd.h"
#include "../klog.h"
#include "core.h"

/* A boot keyboard's report: one byte of modifiers, one reserved, and up to six
 * usages of keys that are down. A boot mouse's: one byte of buttons and two
 * signed movements, with a wheel byte on devices that have one. */
#define KEYBOARD_REPORT 8
#define MOUSE_REPORT    3
#define KEYS_IN_REPORT  6

/* A boot keyboard's output report: one byte, an LED a bit. */
#define REPORT_NUM_LOCK    0x01
#define REPORT_CAPS_LOCK   0x02
#define REPORT_SCROLL_LOCK 0x04

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

/* What each device was reporting last time, one per entry of the core's
 * device table, indexed from one. */
struct hid_state {
    uint8_t modifiers;
    uint8_t keys[KEYS_IN_REPORT];
    struct input_keyboard kb; /* a keyboard's keys in the input layer */
    int keyboard;             /* a keyboard is attached, from `hid_attach` to `hid_gone` */
    int leds_shown;           /* the output report last taken, -1 before one */
    int leds_failed;          /* a report failed since the keyboard was attached */
    char name[24];            /* its name, "USB keyboard N" */
    char phys[24];            /* its EVIOCGPHYS, "usb-N/input0" */
};

static struct hid_state states[USB_MAX_DEVICES + 1];

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

void hid_attach(int device, int kind, uint16_t vendor, uint16_t product)
{
    struct hid_state *state;

    if (device <= 0 || device > USB_MAX_DEVICES || kind != HID_KEYBOARD) {
        return;
    }
    state = &states[device];
    state->keyboard = 1;
    state->leds_shown = -1;
    state->leds_failed = 0;
    if (state->kb.dev) {
        return;
    }
    state->name[0] = 0;
    append(state->name, sizeof(state->name), "USB keyboard ");
    append_number(state->name, sizeof(state->name), device);
    state->phys[0] = 0;
    append(state->phys, sizeof(state->phys), "usb-");
    append_number(state->phys, sizeof(state->phys), device);
    append(state->phys, sizeof(state->phys), "/input0");
    input_keyboard_attach(&state->kb, state->name, state->phys, EVDEV_BUS_USB, vendor, product);
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

void hid_report(int device, int kind, const uint8_t *report, int length)
{
    struct hid_state *state;

    if (device <= 0 || device > USB_MAX_DEVICES) {
        return;
    }
    state = &states[device];

    if (kind == HID_KEYBOARD) {
        if (length < KEYBOARD_REPORT) {
            return;
        }
        keyboard_report(state, report);
        return;
    }

    if (kind == HID_MOUSE) {
        if (length < MOUSE_REPORT) {
            return;
        }
        /* Negated: a USB report's Y is positive toward the user. */
        input_mouse((int)(int8_t)report[1], -(int)(int8_t)report[2], report[0] & 0x07);
        return;
    }
}

int hid_led_report(int device)
{
    struct hid_state *state;
    unsigned int wanted = input_leds();
    int report = 0;

    if (device <= 0 || device > USB_MAX_DEVICES) {
        return -1;
    }
    state = &states[device];
    if (!state->keyboard || state->leds_failed) {
        return -1;
    }
    if (wanted & LED_NUM) {
        report |= REPORT_NUM_LOCK;
    }
    if (wanted & LED_CAP) {
        report |= REPORT_CAPS_LOCK;
    }
    if (wanted & LED_SCR) {
        report |= REPORT_SCROLL_LOCK;
    }
    return report == state->leds_shown ? -1 : report;
}

void hid_led_sent(int device, int report, int ok)
{
    struct hid_state *state;

    if (device <= 0 || device > USB_MAX_DEVICES) {
        return;
    }
    state = &states[device];
    if (!ok) {
        if (!state->leds_failed) {
            klog(KLOG_WARN, "usb", "hid: keyboard %d: the LED report failed", device);
        }
        state->leds_failed = 1;
        return;
    }
    state->leds_shown = report;
}

void hid_gone(int device)
{
    struct hid_state *state;
    int i;

    if (device <= 0 || device > USB_MAX_DEVICES) {
        return;
    }
    state = &states[device];
    input_keyboard_detach(&state->kb);
    state->keyboard = 0;
    state->modifiers = 0;
    for (i = 0; i < KEYS_IN_REPORT; i++) {
        state->keys[i] = 0;
    }
}
