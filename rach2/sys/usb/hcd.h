/*
 * hcd.h: the types the xHCI driver shares with the USB core and the class
 * drivers: speeds, endpoints, transfers, completions and a device's place in
 * the tree.
 *
 * Nothing in the stack waits after bring-up. The pump drains each
 * controller's events from the timer tick and from the controller's
 * interrupt, both with interrupts off, so an operation that spun on a
 * controller would stop the clock and the scheduler with it. A transfer is
 * queued by `xhci_submit` and its end reported by `xhci_poll`; a port reset
 * and each controller command are begun by one call and found finished by
 * another, and the caller advances as a state machine.
 *
 * A controller runs one command at a time, which lets one command ring and
 * one input context serve every device on it; the core gives the command ring
 * to one device at a time, when `xhci_command_free` says it is free.
 *
 * A device handle is the slot number the controller gave out, from one. An
 * endpoint handle is the endpoint's device context index: 1 for endpoint
 * zero, twice the endpoint number for an OUT endpoint and one more for an IN
 * one.
 */

#ifndef _USB_HCD_H_
#define _USB_HCD_H_

#include "../lib/kint.h"

/* A device's speed, as the controller reports it. The numbers are the ones
 * xHCI uses in its port status and slot context, so a driver for another
 * controller translates into these rather than the other way round. */
#define USB_SPEED_FULL  1
#define USB_SPEED_LOW   2
#define USB_SPEED_HIGH  3
#define USB_SPEED_SUPER 4

/* An endpoint's transfer type, the low two bits of its descriptor's
 * `bmAttributes`. Isochronous, 1, has no name here: no driver in this kernel
 * uses it. */
#define USB_EP_CONTROL   0
#define USB_EP_BULK      2
#define USB_EP_INTERRUPT 3

/* Which way the data goes. A control endpoint is both, and the direction of
 * each stage comes from its setup packet instead. */
#define USB_DIR_OUT 0
#define USB_DIR_IN  1

/** What a port, a root port or a hub's, reports about itself. */
struct usb_port_status {
    int connected; /* a device is attached */
    int enabled;   /* the port is enabled and may carry transfers */
    int speed;     /* one of the `USB_SPEED_` values, when connected */
};

/** An endpoint as the core describes it to `xhci_endpoint_open`. */
struct usb_endpoint {
    int number;     /* 0 to 15, from the endpoint descriptor */
    int direction;  /* `USB_DIR_IN` or `USB_DIR_OUT` */
    int type;       /* one of the `USB_EP_` values */
    int max_packet; /* from the endpoint descriptor */
    int interval;   /* the descriptor's `bInterval`, unconverted */
};

/*
 * One transfer. A control transfer has its eight setup bytes and, when it has
 * a data stage, a buffer; every other kind has a buffer alone. The buffer is
 * the caller's and must stay valid until the transfer is retired
 * (`xhci_td_retired`), which may be after its completion: the controller
 * reads and writes it in place, except a piece that a controller limited to
 * 32-bit addresses cannot reach, which moves through a page of the DMA pool.
 */
struct usb_transfer {
    const uint8_t *setup; /* eight bytes, or NULL for a non-control transfer */
    uint8_t *buffer;      /* NULL for a transfer with no data stage */
    uint32_t length;      /* bytes to move; for a control transfer, the data stage's */
    int direction;        /* `USB_DIR_IN` or `USB_DIR_OUT` for the data stage */
    /*
     * A number the caller chooses, given back in the completion. A caller
     * that gives up on a transfer at its deadline and reuses the buffer, as
     * one does for a device pulled out mid-transfer, tells the late
     * completion of the old transfer from the one it waits for by this
     * number. A caller with nothing to tell apart passes zero.
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

/* The completion code of an Enable Slot when every device slot the controller
 * has is in use. The command fails, and the core waits for a slot to come
 * free and tries again. */
#define USB_NO_SLOTS 9

/** What `xhci_command_done` reports about the command in flight. */
struct usb_command_result {
    int status; /* `USB_XFER_OK` or `USB_XFER_FAILED` */
    int code;   /* the controller's own completion code, for the record */
    int device; /* the event's slot: after an Enable Slot, the new device handle */
};

/** What `xhci_poll` reports about a transfer that ended. */
struct usb_completion {
    /* The controller the transfer was on. Slot numbers start at one on every
     * controller, so a machine with two of them has two devices called slot
     * one, and a completion says which. */
    struct xhci_ctrl *hc;
    int device;          /* the device handle the transfer was submitted for */
    int endpoint;        /* the endpoint handle */
    int status;          /* one of the `USB_XFER_` values */
    int code;            /* the controller's own completion code, for the record */
    uint32_t length;     /* bytes the transfer asked for */
    uint32_t residual;   /* what the controller did not move, so length less this
                          * is what moved */
    uint32_t generation; /* the transfer's `generation` */
};

/* The tiers of hubs below a root port, and the ports of a hub this kernel
 * keeps state for. The route string has four bits for each tier, five tiers
 * in twenty bits. A port above 15 does not fit its four bits and is written
 * as 15, which the specification allows for a low- or full-speed device
 * directly below its high-speed hub, whose translator port number, eight bits
 * wide, gives the port instead. A hub's change bitmap is kept in one 32-bit
 * word, bit 0 for the hub and a bit for each port, which bounds the ports at
 * thirty-one. */
#define USB_TREE_TIERS   5
#define USB_HUB_PORT_MAX 31

/*
 * Where a device is in the tree. The core fills it in, and the controller
 * driver writes it into the device's slot context.
 *
 * `root_port` is the controller's port the device is under, numbered from
 * zero, whatever hubs are between. `depth` is how many hubs are above the
 * device, zero on a root port. `route` is the route string: the port on the
 * hub nearest the root in the lowest four bits, numbered from one, and the
 * port on each hub below it in the next four. A low- or full-speed device
 * below a high-speed hub has the handle of the nearest high-speed hub above it
 * in `tt_device`, the port on that hub its path goes through in `tt_port`,
 * and in `tt_multi` whether that hub has one translator per port; every other
 * device has zeros there.
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
 * An operations table for a second controller driver. No code uses it: the
 * core and the class drivers call the `xhci_` functions of `xhci.h`, which
 * take the controller as their first argument, as these members do not. Every
 * member returns 0 on success and a negative number on failure, except where
 * its comment says otherwise, and none of them waits.
 *
 * `device_address` is here rather than in the core because xHCI addresses a
 * device with a command of its own instead of the standard request, and a
 * core that sent the request would be writing one controller's protocol into
 * a layer that is meant not to know it. A controller whose bus takes the
 * request sends it inside this call.
 */
struct usb_hcd {
    /** How many root ports the controller has. */
    int (*ports)(void);

    /** What a root port reports now. */
    int (*port_status)(int port, struct usb_port_status *out);

    /** Begin resetting a port, after the connect has been debounced. */
    int (*port_reset_begin)(int port);

    /** 1 when the reset finished, 0 while it has not, negative on failure. */
    int (*port_reset_done)(int port);

    /**
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

    /** Take an endpoint handle on a device; negative on failure. */
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
