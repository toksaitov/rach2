#ifndef _USB_CORE_H_
#define _USB_CORE_H_

#include "../lib/kint.h"
#include "hcd.h"

/*
 * core.h: devices, their descriptors, and the state machine that brings one
 * from a connected port to a configured device.
 *
 * Nothing here touches a register. What it knows is the standard: the requests
 * every device answers, the shape of a descriptor, and which interface of a
 * configuration this kernel has a driver for. What it does not know is how any
 * of that reaches the wire, which is the host controller's business.
 */

/* How many devices this kernel holds at once. The tables are fixed, like every
 * other table in this kernel, and four is more than the machines here have
 * room for on their root ports without a hub. */
#define USB_MAX_DEVICES 4

/* The largest descriptor this kernel reads. A configuration descriptor with
 * one interface and one endpoint is 25 bytes; a device with several interfaces
 * can be longer, and what does not fit is not read. */
#define USB_DESC_MAX 64

/* The classes this kernel knows by number. */
#define USB_CLASS_HID       3
#define USB_SUBCLASS_BOOT   1
#define USB_PROTOCOL_KBD    1
#define USB_PROTOCOL_MOUSE  2

/** What the configuration descriptor said about the interface this kernel can
 *  drive, and about the endpoint it reports on. */
struct usb_interface_info {
    int found;         /* whether such an interface was there at all */
    int number;        /* the interface's number, for SET_PROTOCOL later */
    int class_code;    /* its class, subclass and protocol */
    int subclass;
    int protocol;
    int ep_number;     /* the interrupt IN endpoint it reports on */
    int ep_max_packet;
    int ep_interval;
    int config_value;  /* the configuration this interface belongs to */
};

/**
 * Read a configuration descriptor and find the first interface this kernel can
 * drive.
 *
 * Walks the descriptors by their own length fields, which is the only way they
 * can be walked, and stops at the first one that does not fit or that claims a
 * length of zero: a device that says a descriptor is zero bytes long would
 * otherwise be walked for ever, and one that says it is longer than what
 * arrived would be read past the end of the buffer.
 *
 * A boot-protocol keyboard or mouse is what it looks for. `out->found` says
 * whether it was there.
 */
void usb_parse_config(const uint8_t *buffer, int length, struct usb_interface_info *out);

/**
 * Bring the stack's devices along by one step. Never waits.
 *
 * `elapsed_ms` is how much time has passed since the last call, which is what
 * the deadlines here are counted against: one from the boot loop, ten from the
 * timer. The caller knows how long its turn is and this module does not.
 */
void usb_core_step(uint32_t elapsed_ms);

/** Start again from nothing: no devices, no ports being enumerated. */
void usb_core_reset(void);

/** Whether nothing is part way through enumeration. */
int usb_core_idle(void);

/** How many devices are configured and running. */
int usb_core_count(void);

/**
 * Describe one device for `/proc/usb`.
 *
 * Returns the number of bytes written, or zero when `index` names no device.
 */
int usb_core_describe(int index, char *out, int size);

#endif
