/*
 * hid.c: a USB keyboard and mouse, in the protocol every firmware uses.
 *
 * The boot protocol is two fixed report layouts, and taking it removes the
 * report-descriptor parser, which is the largest part of a HID stack and the
 * part with nothing to teach about USB.
 *
 * The work here is the difference between what USB sends and what this kernel
 * already has. A HID report is a state: these keys are down now. A scancode is
 * an event: this key went down, that one came up. So each report is compared
 * with the one before it, and the difference is what the input layer is given,
 * in the set-1 codes the PS/2 keyboard produces. No code downstream has to
 * tell the two kinds of keyboard apart.
 */

#include "hid.h"

#include "usb.h"

#include "../input.h"
#include "../klog.h"
#include "core.h"

/* A boot keyboard's report: one byte of modifiers, one reserved, and up to six
 * usages of keys that are down. A boot mouse's: one byte of buttons and two
 * signed movements, with a wheel byte on devices that have one. */
#define KEYBOARD_REPORT 8
#define MOUSE_REPORT    3
#define KEYS_IN_REPORT  6

/*
 * The usages this kernel has a scancode for. A value of zero is a key it does
 * not name; a value with 0xE000 set is one whose scancode is the two-byte
 * extended form, which is how the arrows and the keypad's twins are told
 * apart from the keys they share a code with.
 */
#define EXT 0xE000u

static const uint16_t usage_to_scancode[0x66] = {
    0,          0,          0,          0,          0x1E,       0x30,       0x2E,       0x20,
    0x12,       0x21,       0x22,       0x23,       0x17,       0x24,       0x25,       0x26,
    0x32,       0x31,       0x18,       0x19,       0x10,       0x13,       0x1F,       0x14,
    0x16,       0x2F,       0x11,       0x2D,       0x15,       0x2C,       0x02,       0x03,
    0x04,       0x05,       0x06,       0x07,       0x08,       0x09,       0x0A,       0x0B,
    0x1C,       0x01,       0x0E,       0x0F,       0x39,       0x0C,       0x0D,       0x1A,
    0x1B,       0x2B,       0x2B,       0x27,       0x28,       0x29,       0x33,       0x34,
    0x35,       0x3A,       0x3B,       0x3C,       0x3D,       0x3E,       0x3F,       0x40,
    0x41,       0x42,       0x43,       0x44,       0x57,       0x58,       0,          0x46,
    0,          EXT | 0x52, EXT | 0x47, EXT | 0x49, EXT | 0x53, EXT | 0x4F, EXT | 0x51, EXT | 0x4D,
    EXT | 0x4B, EXT | 0x50, EXT | 0x48, 0x45,       EXT | 0x35, 0x37,       0x4A,       0x4E,
    EXT | 0x1C, 0x4F,       0x50,       0x51,       0x4B,       0x4C,       0x4D,       0x47,
    0x48,       0x49,       0x52,       0x53,       0x56,       EXT | 0x5D};

/* The modifier byte's bits, in order, and the scancode each sends. Right Ctrl
 * and right Alt are the extended form of the left-hand codes; right Shift and
 * each GUI key have codes of their own. */
static const uint16_t modifier_scancode[8] = {0x1D,       0x2A, 0x38,       EXT | 0x5B,
                                              EXT | 0x1D, 0x36, EXT | 0x38, EXT | 0x5C};

/* What each device was reporting last time, one per entry of the core's
 * device table, indexed from one. */
struct hid_state {
    int used; /* it has reported since it last went */
    uint8_t modifiers;
    uint8_t keys[KEYS_IN_REPORT];
};

static struct hid_state states[USB_MAX_DEVICES + 1];

/**
 * Send a scancode, pressed or `released`, in the two-byte form when `code`
 * has EXT set, then take the third of the four stamps the latency record
 * prints. A usage with no scancode, zero in the table, sends nothing.
 */
static void send_code(uint16_t code, int released)
{
    uint8_t byte = (uint8_t)(code & 0xFFu);

    if (byte == 0) {
        return;
    }
    if (code & EXT) {
        input_key(0xE0);
    }
    input_key((uint8_t)(released ? (byte | 0x80u) : byte));
    usb_stamp_wake();
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
            send_code(modifier_scancode[i], 0);
        } else if (!(modifiers & bit) && (state->modifiers & bit)) {
            send_code(modifier_scancode[i], 1);
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

        if (usage > 0x03u && usage < 0x66u && !held(state->keys, usage)) {
            send_code(usage_to_scancode[usage], 0);
        }
    }
    for (i = 0; i < KEYS_IN_REPORT; i++) {
        uint8_t usage = state->keys[i];

        if (usage > 0x03u && usage < 0x66u && !held(keys, usage)) {
            send_code(usage_to_scancode[usage], 1);
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
    state->used = 1;

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
        /* A USB mouse's Y grows downward, the PS/2 queue's upward. */
        input_mouse((int)(int8_t)report[1], -(int)(int8_t)report[2], report[0] & 0x07);
        return;
    }
}

void hid_gone(int device)
{
    struct hid_state *state;
    int i;

    if (device <= 0 || device > USB_MAX_DEVICES) {
        return;
    }
    state = &states[device];
    if (!state->used) {
        return;
    }

    for (i = 0; i < 8; i++) {
        if (state->modifiers & (1u << i)) {
            send_code(modifier_scancode[i], 1);
        }
    }
    for (i = 0; i < KEYS_IN_REPORT; i++) {
        uint8_t usage = state->keys[i];

        if (usage > 0x03u && usage < 0x66u) {
            send_code(usage_to_scancode[usage], 1);
        }
    }

    state->modifiers = 0;
    for (i = 0; i < KEYS_IN_REPORT; i++) {
        state->keys[i] = 0;
    }
    state->used = 0;
}
