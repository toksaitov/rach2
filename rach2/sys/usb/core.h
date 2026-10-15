/*
 * core.h: devices, their descriptors, and the state machine that brings one
 * from a connected port to a configured device handed to its class driver.
 *
 * Nothing here touches a register. What it knows is the standard: the requests
 * every device answers, the shape of a descriptor, and which interface of a
 * configuration this kernel has a driver for. What it does not know is how any
 * of that reaches the wire, which is the host controller's business.
 *
 * The calls run in the USB pump (`usb.c`), with interrupts off, unless their
 * comments say otherwise.
 */
#ifndef _USB_CORE_H_
#define _USB_CORE_H_

#include "../lib/kint.h"
#include "hcd.h"

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
 * other protocol byte a stick may have, 0x62, is UAS, a different transport
 * this kernel does not drive: a stick that offers only UAS is recorded as a
 * device with no driver. */
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

/** What a configuration descriptor says about the first interface in it this
 *  kernel can drive, and about that interface's endpoints. */
struct usb_interface_info {
    int found;      /* whether such an interface was there at all */
    int kind;       /* one of the `USB_KIND_` values */
    int number;     /* the interface's number, for SET_PROTOCOL, a hub's
                     * SET_INTERFACE and a stick's class requests */
    int class_code; /* its class, subclass and protocol */
    int subclass;
    int protocol;
    int ep_number; /* the interrupt IN endpoint a keyboard, mouse or hub
                    * reports on, or the bulk IN endpoint a stick answers on */
    int ep_max_packet;
    int ep_interval; /* the descriptor's bInterval, unconverted */
    /* A stick's bulk OUT endpoint; zero on an interface of another kind. */
    int ep_out_number;
    int ep_out_max_packet;
    int config_value; /* the configuration's bConfigurationValue */
};

/**
 * Find the first interface this kernel can drive in the configuration
 * descriptor `buffer`, `length` bytes, and its endpoints.
 *
 * An interface on alternate setting zero is a candidate when it is a boot
 * keyboard or mouse, a hub (class 9, protocol 0 to 3), or SCSI storage over
 * Bulk-Only. A keyboard, mouse or hub needs an interrupt IN endpoint, and a
 * stick a bulk IN and a bulk OUT endpoint, the first of each. A candidate
 * whose endpoints do not all follow it before the next interface is dropped
 * and the walk goes on. `out` is cleared first; `out->found` says whether an
 * interface was found. A configuration value of zero, or a buffer shorter
 * than the nine-byte header, finds nothing.
 *
 * The descriptors are walked by their own length fields, which is the only
 * way they can be walked, and the walk stops at the first one shorter than two
 * bytes or longer than what is left: a device that says a descriptor is zero
 * bytes long would otherwise be walked for ever, and one that says it is
 * longer than what arrived would be read past the end of the buffer.
 */
void usb_parse_config(const uint8_t *buffer, int length, struct usb_interface_info *out);

/**
 * Bring the stack's devices along by one pass. Never waits.
 *
 * A pass drains each controller's completions to the stick driver, the hub
 * driver and the devices they name; takes down each device whose port has
 * gone empty; gives an entry to the device on each connected root or hub
 * port that has none and was not refused; moves every device one step; and
 * then steps the stick driver, the hub driver and the disk layer
 * (`usbdisk_step`). Called from the pump only.
 *
 * `elapsed_ms` is how much time has passed since the last call, which is what
 * the deadlines here are counted against: zero from the controller's
 * interrupt, ten from the timer's tick. The caller knows how long its turn is
 * and this module does not.
 */
void usb_core_step(uint32_t elapsed_ms);

struct xhci_ctrl;

/*
 * A port's operations. A port belongs to the root controller or to a hub,
 * and the enumeration state machine does not care which: a device record
 * has the operations its port answers to.
 *
 * `hc` is the controller the port's device is on, which a root port needs
 * and a hub's port ignores: a hub reaches its own ports through transfers to
 * itself, and its device record has its controller. `owner` is the hub's
 * device index for a hub port and unused for a root port; `port` is numbered
 * from zero in both cases, and the hub's operations add one when they talk to
 * the hub. `status` fills `out` and returns 0, or -1 when the port cannot be
 * read; `settled` is 1 once a connection has held long enough to reset;
 * `reset_begin` returns 0 when the reset began; `reset_done` returns 1 when
 * the reset finished, 0 while it runs and -1 when it failed.
 */
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
 * now, and only then is a device that is there given an entry, to be
 * debounced and enumerated. The port's refusal is forgotten. `hub` is the
 * hub's device index, `port` numbered from zero; nothing is done for a hub
 * that is not running.
 */
void usb_core_hub_port(int hub, int port, int connected);

/**
 * Tell the controller, for the hub driver, that the device at `index` is a
 * hub with `ports` ports, whether it has a translator per port (`multi_tt`),
 * and its think time, through its controller's command turn. Called on each
 * pass until it answers: returns 0 while the turn is another device's or the
 * command is in flight, 1 when it finished, -1 when it was refused or failed
 * or the device is not running.
 */
int usb_core_hub_tell(int index, int ports, int multi_tt, int think_time);

/**
 * The hub driver gives up the running hub at `index`: the core fails it with
 * `why` in the record, takes down what is below it, remembers its port as
 * refused and closes its slot.
 */
void usb_core_hub_failed(int index, const char *why);

/**
 * The location of the device at entry `index` in `out`. Returns 0, or -1 for
 * an index whose entry is free, which is how the hub driver learns that a
 * hub's entry has closed.
 */
int usb_core_location(int index, struct usb_location *out);

/**
 * Start again from nothing: no devices, no ports being enumerated, every
 * record's frame given back. Called at bring-up, before the pump runs.
 */
void usb_core_reset(void);

/**
 * Clear a halted endpoint for a class driver, through the core's own turn at
 * the controller's one command: `usb_core_endpoint_quiesce`, whose reset of a
 * halted endpoint clears the controller's halt. The device's own halt is the
 * caller's, with CLEAR_FEATURE(ENDPOINT_HALT).
 *
 * 1 when the endpoint is usable again, 0 while the controller has not
 * answered, -1 when it refused or the device is not one the core has.
 */
int usb_core_clear_halt(struct xhci_ctrl *hc, int device, int endpoint);

/**
 * Stop an endpoint so that no block of its transfers can be fetched again:
 * Stop Endpoint on a running one or Reset Endpoint on a halted one, then
 * Set TR Dequeue at the driver's next block, each awaited and required to
 * succeed, on the controller's command turn, which is held across both. A
 * stopped endpoint, or one in the Error state, needs only the dequeue.
 * Nothing is submitted on the endpoint from the first call on.
 *
 * 0 while it is pending, 1 when both answered with success and the
 * endpoint's transfers are retired, -1 on a refusal, a failure or a
 * deadline, with nothing retired, or for a device the core does not have.
 * One step a call; never spins. The core uses it for a keyboard's or mouse's
 * halted report endpoint, and the stick driver for its endpoints.
 */
int usb_core_endpoint_quiesce(struct xhci_ctrl *hc, int device, int endpoint);

/**
 * Give up a running device whose class driver cannot reach it any more.
 *
 * The device is closed as if it had left, `why` in the record; its port still
 * shows a connection, so the next pass enumerates it again through a port
 * reset, which is what a replug by hand does. This is for a device that stops
 * answering while its port still reports a connection, such as a stick behind
 * a SuperSpeed hub whose link stays up. Nothing is done for a device not
 * running on `hc` at slot `device`.
 */
void usb_core_device_drop(struct xhci_ctrl *hc, int device, const char *why);

/**
 * Whether no device is being enumerated, recovered or closed. A device
 * waiting for a free slot counts as idle, since it may wait for ever. Read at
 * boot by `usb_init`, while the timer runs the pump.
 */
int usb_core_idle(void);

/** How many devices are configured and running; read at boot by `usb_init`. */
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
 * Describe the running device at table entry `index` for `/proc/usb`, as one
 * line in `out`, `size` bytes with its terminator: `port PATH SPEED
 * VENDOR:PRODUCT class C protocol P`, with PATH the root port and then each
 * hub's port, from 1, joined by dots, and the identifiers in hex.
 *
 * Returns the number of bytes written, or zero when `index` names no running
 * device or `size` is less than 8. Called in process context, with no lock
 * taken.
 */
int usb_core_describe(int index, char *out, int size);

#endif
