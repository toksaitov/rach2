/*
 * hid.h: the boot-protocol keyboard and mouse.
 *
 * A human-interface device describes its reports in a language of its own, and
 * a driver that reads that language is the largest part of a USB HID stack.
 * Every firmware skips it the same way this does: a keyboard and a mouse are
 * required to offer a boot protocol, whose two report layouts are fixed and
 * need no describing. A device that offers no boot protocol is refused, and
 * the Readme says so.
 */
#ifndef _USB_HID_H_
#define _USB_HID_H_

#include "../lib/kint.h"

/** A device that reports as a boot keyboard. */
#define HID_KEYBOARD 1

/** A device that reports as a boot mouse. */
#define HID_MOUSE 2

/**
 * Say that a device runs in the boot protocol, `device` numbered as for
 * `hid_report`: a keyboard is registered as the event device "USB keyboard
 * N", N being `device`, with its `vendor` and `product`. Nothing for a
 * mouse, or for a keyboard registered already.
 */
void hid_attach(int device, int kind, uint16_t vendor, uint16_t product);

/**
 * Take one report from a device, in the USB pump.
 *
 * `device` is the core's table entry for the device plus one, 1 to
 * USB_MAX_DEVICES, so its previous report can be found; any other value is
 * ignored. `kind` is `HID_KEYBOARD` or `HID_MOUSE`, and `length` is how much
 * actually arrived. A keyboard's report goes to the input layer as the key
 * events of the keys that went down or came up since its last report; a
 * mouse's goes as one movement with its buttons. A report shorter than its
 * layout is dropped rather than read past.
 */
void hid_report(int device, int kind, const uint8_t *report, int length);

/**
 * The output report keyboard `device`, numbered as for `hid_report`, is to
 * be sent so that its LEDs show what the input layer wants (`input_leds`):
 * bit 0 Num Lock, bit 1 Caps Lock and bit 2 Scroll Lock, the boot
 * keyboard's layout. Returns the byte, or -1 when the keyboard shows it
 * already, has failed a report since it was attached, or is not an
 * attached keyboard. A keyboard just attached shows nothing known, so it is
 * sent the LEDs at once. Called from the USB stack's step.
 */
int hid_led_report(int device);

/**
 * Say that the output report `report` was sent to keyboard `device`, `ok`
 * when the keyboard took it. After a failure the keyboard is sent no report
 * until it is attached again.
 */
void hid_led_sent(int device, int report, int ok);

/**
 * Say that a device has gone, `device` numbered as for `hid_report`.
 *
 * Everything it was holding down is released, its repeat stops and its event
 * device is taken away (`input_keyboard_detach`). Without this a key held
 * while the keyboard is unplugged stays down for ever, because the release
 * that would have lifted it is on a device that is no longer there.
 */
void hid_gone(int device);

#endif
