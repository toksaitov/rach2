#ifndef _USB_HCD_H_
#define _USB_HCD_H_

#include "../lib/kint.h"

/*
 * hcd.h: what a USB host controller provides, and all the layers above it may
 * assume.
 *
 * One implementation exists, `xhci.c`. The interface is written for two: the
 * ATA and AHCI drivers already share one block interface here, and the same
 * shape is used again so that a second controller, or a mass-storage class
 * driver, can be added without changing what the first one does.
 *
 * Nothing here waits. The kernel drains completions from the timer handler,
 * which runs with interrupts off, so an operation that spun on a controller
 * would stop the clock and the scheduler with it. `submit` queues work and
 * returns; `poll` reports what finished. A port reset is two calls, one to
 * begin it and one to ask whether it finished. Everything that takes time is
 * split the same way, and the caller advances as a state machine.
 *
 * Four of these operations are commands the controller executes rather than
 * register writes the caller can see the effect of: taking a device handle,
 * addressing a device, opening an endpoint, and resetting a halted one. Each
 * of those issues one command and returns; `command_done` reports the one in
 * flight. A caller may have one command outstanding at a time, which is what
 * lets the whole stack hold a single command ring and no queue of its own,
 * and is enough for a machine whose devices are a keyboard and a mouse.
 *
 * Handles are small integers into fixed tables. There is no allocation
 * anywhere in the stack, so a handle is an index and its lifetime is the
 * caller's business.
 */

/* A device's speed, as the controller reports it. The numbers are the ones
 * xHCI uses in its port status and slot context, so a driver for another
 * controller translates into these rather than the other way round. */
#define USB_SPEED_FULL  1
#define USB_SPEED_LOW   2
#define USB_SPEED_HIGH  3
#define USB_SPEED_SUPER 4

/* What an endpoint carries. Isochronous is absent because nothing in this
 * kernel has a use for it, and an interface that named it would have to
 * refuse it everywhere. */
#define USB_EP_CONTROL   0
#define USB_EP_BULK      2
#define USB_EP_INTERRUPT 3

/* Which way the data goes. A control endpoint is both, and the direction of
 * each stage comes from its setup packet instead. */
#define USB_DIR_OUT 0
#define USB_DIR_IN  1

/** What a root port says about itself. */
struct usb_port_status {
    int connected; /* a device is attached */
    int enabled;   /* the port is enabled and may carry transfers */
    int changed;   /* something changed since this was last asked */
    int speed;     /* one of the `USB_SPEED_` values, when connected */
};

/** An endpoint as the class driver describes it, before it exists. */
struct usb_endpoint {
    int number;     /* 0 to 15, from the endpoint descriptor */
    int direction;  /* `USB_DIR_IN` or `USB_DIR_OUT` */
    int type;       /* one of the `USB_EP_` values */
    int max_packet; /* from the endpoint descriptor */
    int interval;   /* the descriptor's `bInterval`, unconverted */
};

/*
 * One piece of work. A control transfer carries its eight setup bytes and
 * optionally a buffer; every other kind carries a buffer alone. The buffer is
 * the caller's and must outlive the transfer, which is why nothing here
 * copies it.
 */
struct usb_transfer {
    const uint8_t *setup; /* eight bytes, or NULL for a non-control transfer */
    uint8_t *buffer;      /* NULL for a transfer with no data stage */
    uint32_t length;      /* bytes the buffer can hold */
    int direction;        /* `USB_DIR_IN` or `USB_DIR_OUT` for the data stage */
    /*
     * A number the caller chooses, given back in the completion. A device
     * that can be pulled out in the middle of a transfer needs it: the
     * caller gives up on a transfer at its deadline and reuses the buffer,
     * and the completion that arrives afterwards names a generation the
     * caller is no longer waiting for, which is how it is told from the one
     * it wants. A caller with nothing to distinguish passes zero.
     */
    uint32_t generation;
};

/* How a transfer ended. `USB_XFER_STALLED` is separate from the other
 * failures because it is the one the caller recovers from rather than gives
 * up on: the endpoint is halted and must be reset before it carries anything
 * again. */
#define USB_XFER_OK      0
#define USB_XFER_SHORT   1 /* completed, but with fewer bytes than asked for */
#define USB_XFER_STALLED 2
#define USB_XFER_FAILED  3

/** What `command_done` reports about the command in flight. */
/* The completion code a controller gives when every device slot it has is
 * taken. It is not a failure of the command: the caller asks again when one
 * comes free. */
#define USB_NO_SLOTS 9

struct usb_command_result {
    int status; /* `USB_XFER_OK`, or a failure from the same set */
    int code;   /* the controller's own completion code, for the record */
    int device; /* the new device handle, when the command took one */
};

/** What `poll` reports about a transfer that finished. */
struct usb_completion {
    int device;          /* the device handle the transfer was submitted for */
    int endpoint;        /* the endpoint handle */
    int status;          /* one of the `USB_XFER_` values */
    int code;            /* the controller's own completion code, for the record */
    uint32_t length;     /* bytes the transfer asked for */
    uint32_t residual;   /* what the controller did not move, so length less this
                          * is what arrived */
    uint32_t generation; /* what the transfer carried */
};

/* How deep a tree goes below the root: the route string has four bits for
 * each hub tier, five tiers in twenty bits. A port number above 15 does not
 * fit a nibble, and the specification writes 15 in its place: below a full-
 * or high-speed hub the packet is routed by the translator's own port
 * number, which has eight bits and carries the real one. So a hub may have
 * more ports than a nibble names, and this is how many of them this kernel
 * keeps state for; the port bitmaps a hub holds are words, which is what
 * bounds it at thirty-one: a bit each for the ports and one for the hub
 * itself is a word exactly. */
#define USB_TREE_TIERS   5
#define USB_HUB_PORT_MAX 31

/*
 * Where a device is in the tree.
 *
 * `root_port` is the controller's port the device is under, numbered from
 * zero, whatever hubs are between. `depth` is how many hubs are above the
 * device, zero on a root port. `route` is the route string the controller
 * writes into the slot context: the port on the nearest hub in the lowest
 * four bits, numbered from one, and each hub above in the next four. A low
 * or full-speed device below a high-speed hub carries the handle of the
 * nearest high-speed hub above it in `tt_device`, the port on that hub in
 * `tt_port`, and whether that hub has one translator per port in
 * `tt_multi`; every other device carries zeros there. The core fills it in
 * and the controller reads it.
 */
struct usb_location {
    int root_port;
    int depth;
    uint32_t route;
    int tt_device;
    int tt_port;
    int tt_multi;
};

/*
 * The controller. Every call returns 0 on success and a negative number on
 * refusal, except where the comment says otherwise, and none of them waits.
 *
 * `device_address` is here rather than in the core because xHCI addresses a
 * device with a command of its own instead of the standard request, and a
 * core that sent the request would be writing one controller's protocol into
 * a layer that is meant not to know it. A controller whose bus really does
 * take the request sends it inside this call.
 */
struct usb_hcd {
    /** How many root ports the controller has. */
    int (*ports)(void);

    /** What a root port reports now. */
    int (*port_status)(int port, struct usb_port_status *out);

    /** Begin resetting a port, after the connect has been debounced. */
    int (*port_reset_begin)(int port);

    /** 1 when the reset finished, 0 while it has not, negative on refusal. */
    int (*port_reset_done)(int port);

    /*
     * Ask the controller for a device handle for a device at `where`, on a
     * reset port. Issues one command; `command_done` reports the handle.
     * Returns 0 when the command was issued, negative when no slot or no
     * command ring was free, or when the location cannot be written.
     */
    int (*device_open)(const struct usb_location *where, int speed);

    /** Give the device an address and its default control endpoint. */
    int (*device_address)(int device);

    /**
     * Tell the controller that `device` is a hub with `ports` downstream
     * ports. A high-speed hub says whether it has one transaction translator
     * for every port (`multi_tt`) and its think time, 0 to 3 for 8, 16, 24
     * and 32 full-speed bit times; a full-speed hub passes zeros for both.
     * Issues one command.
     */
    int (*device_hub)(int device, int ports, int multi_tt, int think_time);

    /** Release a device handle and everything the controller holds for it. */
    void (*device_close)(int device);

    /** Take an endpoint handle on a device; negative on refusal. */
    int (*endpoint_open)(int device, const struct usb_endpoint *endpoint);

    /** 1 when the command in flight finished, 0 while it has not. */
    int (*command_done)(struct usb_command_result *out);

    /** Release an endpoint handle. */
    void (*endpoint_close)(int device, int endpoint);

    /** Clear a halted endpoint and make its queue usable again. */
    int (*endpoint_reset)(int device, int endpoint);

    /** Queue one transfer. It is reported by `poll` when it ends. */
    int (*submit)(int device, int endpoint, const struct usb_transfer *transfer);

    /** 1 when a completion was reported, 0 when none was waiting. */
    int (*poll)(struct usb_completion *out);
};

#endif
