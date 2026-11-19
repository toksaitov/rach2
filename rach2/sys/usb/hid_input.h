/*
 * hid_input.h: a parsed HID device's usages as event devices, Linux's
 * hid-input.c, and its reports read as hid-core.c reads them.
 */
#ifndef _USB_HID_INPUT_H_
#define _USB_HID_INPUT_H_

#include "../evdev.h"
#include "../input.h"
#include "../lib/kint.h"
#include "hid_parse.h"

/* The input devices one HID device makes at most: one per application
 * collection, as hid-generic makes them. */
#define HID_MAX_INPUTS 8

/* The bytes of a device's name with its terminator, Linux's `hid->name`. */
#define HID_NAME_MAX 128

/* The bytes of its serial with its terminator, Linux's `hid->uniq`. */
#define HID_UNIQ_MAX 64

/* One input device, Linux's `struct hid_input`, with the capabilities its
 * usages were mapped to before its event device is registered. */
struct hid_input {
    uint32_t application; /* the application collection's usage */
    char name[HID_NAME_MAX];
    struct evdev *dev;        /* its event device, or NULL */
    struct input_keyboard kb; /* a keyboard's: the input layer registers it */
    uint32_t ev;              /* the event types it reports, a bit each */
    uint8_t mouse;            /* REL_X and REL_Y: it moves the mouse queue too */
    uint8_t touched;          /* the report being read has a field of it */
    uint8_t keys[EVDEV_KEY_BYTES];
    uint8_t down[EVDEV_KEY_BYTES]; /* the keys down now, for MSC_SCAN */
    uint8_t rel[2];
    uint8_t abs[8];
    uint8_t led[2];
    uint8_t msc[1];
    unsigned int buttons;       /* the mouse queue's packet under way: its buttons, */
    int dx;                     /* its motion to the right, */
    int dy;                     /* and toward the user */
    unsigned int mt_flags;      /* INPUT_MT_* for a multitouch application */
    uint16_t mt_first, mt_last; /* the contact groups in this packet */
    int mt_count;               /* frame size in the packet, or -1 if absent */
    int mt_left;                /* contacts still due in a counted frame */
    unsigned int mt_buttons;    /* the buttons described by the touch application */
};

struct hid_mt_contact;

/* A HID device's parsed descriptor and its input devices. */
struct hid_dev {
    struct hid_desc desc;
    char name[HID_NAME_MAX]; /* "Manufacturer Product", as usbhid writes it */
    const char *phys;        /* EVIOCGPHYS, kept by its pointer */
    char uniq[HID_UNIQ_MAX]; /* EVIOCGUNIQ: the serial string, "" for none */
    uint16_t vendor;
    uint16_t product;
    uint16_t version; /* bcdHID, which usbhid gives as the version */
    uint32_t ninputs;
    uint8_t *inbuf;                  /* a report padded to its full length, `insize` bytes */
    uint32_t insize;                 /* the longest input report with its ID, plus slack */
    uint8_t *outbuf;                 /* two of the longest output or Feature report, for hid.c */
    uint32_t outsize;                /* the longest output or Feature report with its ID */
    int multiplier;                  /* a Resolution Multiplier report to send, or -1 */
    struct hid_mt_contact *contacts; /* one per collection, only for multitouch */
    struct hid_input inputs[HID_MAX_INPUTS];
};

/**
 * Map the usages of `hid`, whose descriptor is parsed, to event devices and
 * register them, as Linux's `hidinput_connect` does for hid-generic: with
 * no application or physical collection of a kind input handles, nothing
 * is made; otherwise one input device for each application, a System
 * Control or Consumer Control one joining a keyboard's, each named "NAME
 * SUFFIX" when the device has more than one application, and an input
 * device that maps nothing is dropped. A keyboard, an input device with
 * keyboard usages, is attached to the input layer, which repeats its keys
 * and gives them to the console; a pointer with REL_X and REL_Y moves the
 * mouse queue as well. Returns the input devices registered, or -1 for
 * none.
 */
int hidinput_connect(struct hid_dev *hid);

/** Take the input devices of `hid` away, releasing what they hold down. */
void hidinput_disconnect(struct hid_dev *hid);

/**
 * Take one input report of `size` bytes, its ID byte first on a device
 * that numbers its reports, as Linux's `hid_input_report`: a report of
 * an unknown ID or of no bytes is dropped, a short one is read as if
 * padded with zeros, each field's elements are compared with the last
 * report's, and the changes go to the input devices as events, each
 * device's ended by SYN_REPORT.
 */
void hid_input_report(struct hid_dev *hid, const uint8_t *data, uint32_t size);

/**
 * Build the output report of `hid`'s LEDs in `out`, of `size` bytes, as
 * Linux's `hidinput_led_worker` builds it: the report that holds the first
 * LED field, and every other field of the report 0. Each LED usage is set
 * from the input device that owns its field: the LEDs programs lit on its
 * event device, and on a keyboard Num, Caps and Scroll Lock from `locks`,
 * the console's, a bit 1 << LED_* each. `*value` is set to the wValue of
 * SET_REPORT, 0x0200 with the report's ID. Returns the report's bytes, its
 * ID byte first when it has one, or -1 when the device has no LED or the
 * report is longer than `size`.
 */
int hidinput_led_report(struct hid_dev *hid, unsigned int locks, uint8_t *out, int size,
                        int *value);

/**
 * Build the Feature report that sets each Resolution Multiplier of `hid`
 * to its logical maximum, as Linux's `hidinput_change_resolution_multipliers`
 * does, in `out` of `size` bytes, and its wValue in `*value`. Returns the
 * report's bytes, or -1 when there is none to send: the device has no
 * multiplier, or its report holds other fields, whose values Linux reads
 * from the device first and this driver does not.
 */
int hidinput_multiplier_report(struct hid_dev *hid, uint8_t *out, int size, int *value);

/** Say whether the device took the multiplier report: its wheels then
 * report in fractions of a detent, which the hi-res axes give as such. */
void hidinput_multiplier_set(struct hid_dev *hid, int ok);

#endif
