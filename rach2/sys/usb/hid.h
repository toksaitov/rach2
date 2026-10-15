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
 * Take one report from a device, in the USB pump.
 *
 * `device` is the core's table entry for the device plus one, 1 to
 * USB_MAX_DEVICES, so its previous report can be found; any other value is
 * ignored. `kind` is `HID_KEYBOARD` or `HID_MOUSE`, and `length` is how much
 * actually arrived. A keyboard's report goes to the input layer as the set-1
 * scancodes of the keys that went down or came up since its last report; a
 * mouse's goes as one movement with its buttons. A report shorter than its
 * layout is dropped rather than read past.
 */
void hid_report(int device, int kind, const uint8_t *report, int length);

/**
 * Say that a device has gone, `device` numbered as for `hid_report`.
 *
 * Everything it was holding down is released. Without this a key held while
 * the keyboard is unplugged stays down for ever, because the release that
 * would have lifted it is on a device that is no longer there. Nothing is
 * done for a device that has not reported since it last went.
 */
void hid_gone(int device);

#endif
