/*
 * hid.h: the USB HID class driver, which reads each HID interface's report
 * descriptor as Linux's usbhid and hid-generic do, and drives a keyboard or
 * mouse whose descriptor it cannot read in the boot protocol.
 */
#ifndef _USB_HID_H_
#define _USB_HID_H_

#include "../lib/kint.h"

/* A device in the boot protocol, by its interface's protocol. */
#define HID_KEYBOARD 1
#define HID_MOUSE    2

/* What the core read of an interface for the driver: its device's identity,
 * bcdHID from its HID descriptor, its device's manufacturer, product and
 * serial strings in UTF-8, each empty when it has none, and its
 * bInterfaceNumber. */
struct hid_ids {
    uint16_t vendor;
    uint16_t product;
    uint16_t version;
    const char *manufacturer;
    const char *product_name;
    const char *serial;
    int interface;
};

/*
 * A HID interface is named by two numbers: `device`, the core's table entry
 * plus one, 1 to USB_MAX_DEVICES, and `iface`, its place among the HID
 * interfaces the core drives on that device, 0 to USB_HID_INTERFACES - 1.
 * Each interface is a device of its own here, as each is one usbhid device
 * on Linux: its own report descriptor, event devices, reports and output
 * reports. A call that names an interface not taken does nothing, or
 * answers as its comment says.
 */

/**
 * Take interface `iface` of `device` in the report protocol, with its report
 * descriptor `rdesc` of `length` bytes, as Linux's usbhid and hid-generic
 * take one: the descriptor is parsed (`hid_parse`), and each application of
 * it that input handles becomes an event device named "MANUFACTURER
 * PRODUCT", or "HID VVVV:PPPP" without strings (`hidinput_connect`), on
 * "usb-DEVICE/inputN", N its bInterfaceNumber. An interface taken already
 * is a state the entry's last device left, and is taken away first, as
 * `hid_gone` takes it. Returns 0; -EINVAL or -E2BIG for a descriptor
 * refused, -ENOMEM when its tables cannot be had, and -ENODEV when it makes
 * no input device, each logged; or -ENODEV, not logged, when it names no
 * interface the table has room for. Nothing is kept on an error, and the
 * core may try the boot protocol.
 */
int hid_attach(int device, int iface, const struct hid_ids *ids, const uint8_t *rdesc, int length);

/**
 * Take interface `iface` of `device` in the boot protocol, as Linux's usbkbd
 * and usbmouse take one: `kind` HID_KEYBOARD is an event device of the
 * keyboard's keys and HID_MOUSE one of five buttons, a wheel and its hi-res
 * axis, each named "MANUFACTURER PRODUCT", or "USB HIDBP Keyboard
 * VVVV:PPPP" or "USB HIDBP Mouse VVVV:PPPP" without strings. An interface
 * taken already is taken away first, as `hid_attach` does. Returns 0;
 * -ENOMEM, logged, when its state cannot be had, and -ENODEV for a `kind`
 * of neither or an interface out of range.
 */
int hid_attach_boot(int device, int iface, int kind, const struct hid_ids *ids);

/**
 * The bytes the core asks a report transfer of an interface for: its longest
 * input report, with its ID byte, in the report protocol; 0 in the boot
 * protocol or for an interface not taken, for the core to ask for its
 * endpoint's packet.
 */
int hid_report_size(int device, int iface);

/**
 * Take one report from interface `iface` of `device`, in the USB pump;
 * `length` is how much arrived. In the report protocol it is read as
 * Linux reads one (`hid_input_report`). In the boot protocol a keyboard's
 * report goes to the input layer as the key events of the keys that went
 * down or came up since its last report, and a mouse's as one packet of
 * its buttons, movement and wheel (`mouse_report`); a boot report shorter
 * than its layout is dropped.
 */
void hid_report(int device, int iface, const uint8_t *report, int length);

/**
 * The bytes the core must give `hid_output_report` for an interface: its
 * longest output or Feature report, with its ID byte, in the report
 * protocol, 1 for a boot keyboard or mouse, and 0 for one not taken.
 */
int hid_output_size(int device, int iface);

/**
 * The next report the core is to send interface `iface` of `device` with
 * SET_REPORT on the device's control endpoint, to that interface, built in
 * `out` of `size` bytes, with the request's wValue, the report's type and
 * ID, in `*value`. First a report that sets each Resolution Multiplier to
 * its maximum, as Linux sends one when it connects a device; then the LEDs
 * the input layer wants (`input_leds`), in the report of the interface's
 * LED field or a boot keyboard's one byte, when they differ from the report
 * last sent. Returns the report's bytes, or -1 for none: nothing differs,
 * the interface has no LED, or its LED report failed since it was taken.
 * Called from the USB stack's step.
 */
int hid_output_report(int device, int iface, uint8_t *out, int size, int *value);

/**
 * Say that the report `hid_output_report` gave last reached the interface,
 * `ok` when the device took it. After an LED report fails the interface is
 * sent none until it is taken again; a failed multiplier report leaves its
 * wheels as they were.
 */
void hid_output_sent(int device, int iface, int ok);

/**
 * Say that a device has gone, `device` numbered as above, with every
 * interface of it.
 *
 * Everything each interface was holding down is released, its repeat stops,
 * its event devices are taken away and its tables and state freed. Without
 * this a key held while the keyboard is unplugged stays down for ever,
 * because the release that would have lifted it is on a device that is no
 * longer there.
 */
void hid_gone(int device);

#endif
