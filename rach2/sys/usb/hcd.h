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
 * split the same way, and the caller is a state machine rather than a
 * straight line.
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
    int number;      /* 0 to 15, from the endpoint descriptor */
    int direction;   /* `USB_DIR_IN` or `USB_DIR_OUT` */
    int type;        /* one of the `USB_EP_` values */
    int max_packet;  /* from the endpoint descriptor */
    int interval;    /* the descriptor's `bInterval`, unconverted */
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
struct usb_command_result {
    int status;  /* `USB_XFER_OK`, or a failure from the same set */
    int device;  /* the new device handle, when the command took one */
};

/** What `poll` reports about a transfer that finished. */
struct usb_completion {
    int device;      /* the device handle the transfer was submitted for */
    int endpoint;    /* the endpoint handle */
    int status;      /* one of the `USB_XFER_` values */
    uint32_t length; /* bytes actually moved, from the controller */
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
     * Ask the controller for a device handle for a device on a reset port.
     * Issues one command; `command_done` reports the handle. Returns 0 when
     * the command was issued, negative when no slot or no command ring was
     * free.
     */
    int (*device_open)(int port, int speed);

    /** Give the device an address and its default control endpoint. */
    int (*device_address)(int device);

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
