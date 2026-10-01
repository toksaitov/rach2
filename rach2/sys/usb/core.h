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

/* How many devices this kernel holds at once. A device's record is a frame
 * taken when it appears and given back when it goes, so this bounds the table
 * of records and not the memory they take: a machine with two controllers and
 * hubs on both has room for more than the root ports hold. */
#define USB_MAX_DEVICES 16

/* The largest descriptor this kernel reads. A configuration descriptor with
 * one interface and one endpoint is 25 bytes; a device with several
 * interfaces and several endpoints runs to hundreds. A device's record is one
 * frame, and this is what is left of that frame for the descriptor, so a
 * configuration is read whole rather than truncated to a header. A device
 * whose configuration is longer than this is refused by name. */
#define USB_DESC_MAX 3072

/* The classes this kernel knows by number. */
#define USB_CLASS_HID      3
#define USB_SUBCLASS_BOOT  1
#define USB_PROTOCOL_KBD   1
#define USB_PROTOCOL_MOUSE 2

/* Mass storage, of which this kernel drives the one transport that every
 * flash drive has: the SCSI transparent command set over Bulk-Only. The
 * other protocol byte a stick may carry, 0x62, is UAS, which is a different
 * transport and is refused by name. */
#define USB_CLASS_STORAGE      8
#define USB_SUBCLASS_SCSI      6
#define USB_PROTOCOL_BULK_ONLY 0x50
#define USB_PROTOCOL_UAS       0x62

/* Which driver an interface belongs to. */
#define USB_KIND_NONE    0
#define USB_KIND_HID     1
#define USB_KIND_STORAGE 2
#define USB_KIND_HUB     3

/* The hub class, from the USB 2.0 specification's chapter 11: class 9, and
 * a protocol that says what translates for the devices below it. */
#define USB_CLASS_HUB           9
#define USB_PROTOCOL_HUB_FS     0 /* full speed, or high speed with no translator */
#define USB_PROTOCOL_HUB_ONE_TT 1
#define USB_PROTOCOL_HUB_MULTI  2
#define USB_PROTOCOL_HUB_SUPER  3

/** What the configuration descriptor said about the interface this kernel can
 *  drive, and about the endpoint it reports on. */
struct usb_interface_info {
    int found;      /* whether such an interface was there at all */
    int kind;       /* one of the `USB_KIND_` values */
    int number;     /* the interface's number, for SET_PROTOCOL and the
                     * class requests a stick's transport is made of */
    int class_code; /* its class, subclass and protocol */
    int subclass;
    int protocol;
    int ep_number; /* the interrupt IN endpoint a keyboard reports on, or
                    * the bulk IN endpoint a stick answers on */
    int ep_max_packet;
    int ep_interval;
    /* A stick's second endpoint. Zero on an interface that has one. */
    int ep_out_number;
    int ep_out_max_packet;
    int config_value; /* the configuration this interface belongs to */
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

/*
 * A port belongs to the root controller or to a hub, and the enumeration
 * state machine does not care which: a device record holds the operations
 * its port answers to. `owner` is the hub's device index for a hub port and
 * unused for a root port; `port` is numbered from zero in both cases, and
 * the hub's operations add one when they talk to the hub.
 */
/*
 * A port's operations. `hc` is the controller the port's device is on, which a
 * root port needs and a hub's port ignores: a hub reaches its own ports
 * through transfers to itself, and its device already names its controller.
 */
struct xhci_ctrl;

struct usb_port_ops {
    int (*status)(struct xhci_ctrl *hc, int owner, int port, struct usb_port_status *out);
    int (*settled)(struct xhci_ctrl *hc, int owner, int port);
    int (*reset_begin)(struct xhci_ctrl *hc, int owner, int port);
    int (*reset_done)(struct xhci_ctrl *hc, int owner, int port);
};

/**
 * A hub reports what is on one of its ports: `connected` says whether a
 * device is there now. A connection change is a change of identity: what
 * was below the port is closed first, depth-first, whatever the status says
 * now, and only then is a device that is there debounced and enumerated.
 * `hub` is the hub's device index, `port` numbered from zero.
 */
void usb_core_hub_port(int hub, int port, int connected);

/**
 * Tell the controller, for the hub driver, that the device at `index` is a
 * hub, through the one command turn the stack has. Returns 0 while the turn
 * is another device's or the command is in flight, 1 when it finished, -1
 * when it was refused or failed.
 */
int usb_core_hub_tell(int index, int ports, int multi_tt, int think_time);

/** The hub driver gives up the hub at `index`, with the reason. */
void usb_core_hub_failed(int index, const char *why);

/**
 * The location of a device, by its index: the hub driver passes it on to the
 * controller for the hub's own words. Returns -1 for an index that names no
 * device.
 */
int usb_core_location(int index, struct usb_location *out);

/** Start again from nothing: no devices, no ports being enumerated. */
void usb_core_reset(void);

/**
 * Clear a halted endpoint for a class driver, through the core's own turn at
 * the controller's one command.
 *
 * 1 when the endpoint is usable again, 0 while the controller has not
 * answered, -1 when it refused or the device is not one the core has.
 */
int usb_core_clear_halt(struct xhci_ctrl *hc, int device, int endpoint);

/**
 * Stop an endpoint so that no block of its transfers can be fetched again:
 * Stop Endpoint on a running one or Reset Endpoint on a halted one, then
 * Set TR Dequeue at the driver's next block, each awaited and required to
 * succeed, on the controller's command turn, which is held across both.
 * 0 while it is pending, 1 when both answered with success and the
 * endpoint's transfers are retired, -1 on a refusal, a failure or a
 * deadline, with nothing retired. One step a call; never spins.
 */
int usb_core_endpoint_quiesce(struct xhci_ctrl *hc, int device, int endpoint);

/**
 * Give up a running device whose class driver cannot reach it any more.
 *
 * The device is closed as if it had left; its port still shows a
 * connection, so the next pass enumerates it again through a port reset,
 * which is what a replug by hand does. `why` goes in the record. A stick
 * that stopped answering behind the notebook's SuperSpeed hub, with the hub
 * reporting its link up, was the case.
 */
void usb_core_device_drop(struct xhci_ctrl *hc, int device, const char *why);

/** Whether nothing is part way through enumeration. */
int usb_core_idle(void);

/** How many devices are configured and running. */
int usb_core_count(void);

/* A running device as /sys/bus/usb/devices names it: its bus, the number
 * of its controller from 1; its ports from the root port down, each from 1,
 * `depth` of them; the controller's slot, which is its device number; and
 * its descriptor's vendor, product and release. */
struct usb_sys_row {
    int bus;
    int depth;
    int ports[USB_TREE_TIERS + 1];
    int devnum;
    uint16_t vendor, product, release;
};

/** Fill `row` for the device of table entry `index`; 0, or -1 when that
 * entry is not a running device. The entry is read with interrupts off, so
 * a device the interrupt side takes away is read whole or not at all. */
int usb_core_row(int index, struct usb_sys_row *row);

/**
 * Describe one device for `/proc/usb`.
 *
 * Returns the number of bytes written, or zero when `index` names no device.
 */
int usb_core_describe(int index, char *out, int size);

#endif
