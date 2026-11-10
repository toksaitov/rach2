/*
 * hid.h: the USB HID class driver, which reads each device's report
 * descriptor as Linux's usbhid and hid-generic do, and drives a keyboard or
 * mouse whose descriptor it cannot read in the boot protocol.
 */
#ifndef _USB_HID_H_
#define _USB_HID_H_

#include "../lib/kint.h"

/* A device in the boot protocol, by its interface's protocol. */
#define HID_KEYBOARD 1
#define HID_MOUSE    2

/* What the core read of a device for the driver: its identity, bcdHID from
 * its HID descriptor, and its manufacturer, product and serial strings in
 * UTF-8, each empty when it has none. */
struct hid_ids {
    uint16_t vendor;
    uint16_t product;
    uint16_t version;
    const char *manufacturer;
    const char *product_name;
    const char *serial;
};

/**
 * Take device `device`, the core's table entry plus one, 1 to
 * USB_MAX_DEVICES, in the report protocol, with its report descriptor
 * `rdesc` of `length` bytes, as Linux's usbhid and hid-generic take one:
 * the descriptor is parsed (`hid_parse`), and each application of it that
 * input handles becomes an event device named "MANUFACTURER PRODUCT", or
 * "HID VVVV:PPPP" without strings (`hidinput_connect`). Returns 0; -EINVAL
 * or -E2BIG for a descriptor refused, -ENOMEM when its tables cannot be
 * had, and -ENODEV when it makes no input device, each logged; nothing is
 * kept then, and the core may try the boot protocol.
 */
int hid_attach(int device, const struct hid_ids *ids, const uint8_t *rdesc, int length);

/**
 * Take device `device`, numbered as for `hid_attach`, in the boot protocol,
 * as Linux's usbkbd and usbmouse take one: `kind` HID_KEYBOARD is an event
 * device of the keyboard's keys and HID_MOUSE one of five buttons, a wheel
 * and its hi-res axis, each named "MANUFACTURER PRODUCT", or "USB HIDBP
 * Keyboard VVVV:PPPP" or "USB HIDBP Mouse VVVV:PPPP" without strings.
 * Nothing for a device taken already.
 */
void hid_attach_boot(int device, int kind, const struct hid_ids *ids);

/**
 * The bytes the core asks a report transfer of `device` for: its longest
 * input report, with its ID byte, in the report protocol; 0 in the boot
 * protocol or for a device not taken, for the core to ask for its
 * endpoint's packet.
 */
int hid_report_size(int device);

/**
 * Take one report from `device`, numbered as for `hid_attach`, in the USB
 * pump; `length` is how much arrived. In the report protocol it is read as
 * Linux reads one (`hid_input_report`). In the boot protocol a keyboard's
 * report goes to the input layer as the key events of the keys that went
 * down or came up since its last report, and a mouse's as one packet of
 * its buttons, movement and wheel (`mouse_report`); a boot report shorter
 * than its layout is dropped. Any other `device` is ignored.
 */
void hid_report(int device, const uint8_t *report, int length);

/**
 * The next report the core is to send `device` with SET_REPORT on its
 * control endpoint, built in `out` of `size` bytes, with the request's
 * wValue, the report's type and ID, in `*value`. First a report that sets
 * each Resolution Multiplier to its maximum, as Linux sends one when it
 * connects a device; then the LEDs the input layer wants (`input_leds`),
 * in the report of the device's LED field or a boot keyboard's one byte,
 * when they differ from the report last sent. Returns the report's bytes,
 * or -1 for none: nothing differs, the device has no LED, or its LED
 * report failed since it was taken. Called from the USB stack's step.
 */
int hid_output_report(int device, uint8_t *out, int size, int *value);

/**
 * Say that the report `hid_output_report` gave last reached `device`, `ok`
 * when the device took it. After an LED report fails the device is sent
 * none until it is taken again; a failed multiplier report leaves its
 * wheels as they were.
 */
void hid_output_sent(int device, int ok);

/**
 * Say that a device has gone, `device` numbered as for `hid_attach`.
 *
 * Everything it was holding down is released, its repeat stops, its event
 * devices are taken away and its tables freed. Without this a key held
 * while the keyboard is unplugged stays down for ever, because the release
 * that would have lifted it is on a device that is no longer there.
 */
void hid_gone(int device);

#endif
