#ifndef _USB_MSC_H_
#define _USB_MSC_H_

#include "../lib/kint.h"

/*
 * msc: the USB mass-storage class driver, over Bulk-Only Transport.
 *
 * A flash drive is a SCSI target reached through two bulk endpoints. Every
 * command is three transfers: a 31-byte command block wrapper out, the data
 * in or out when there is any, and a 13-byte status wrapper in. This driver
 * is a state machine over those three, stepped from the one pump the timer
 * and the controller's interrupt share, with one command in flight per
 * device.
 *
 * Nothing here waits. The block layer's sleeping side waits on the request,
 * not on this driver, and this driver runs entirely inside the pump.
 */

/** How many sticks this kernel drives at once. */
#define MSC_MAX_DEVICES 2

/**
 * Take a device the core has enumerated as Bulk-Only mass storage.
 *
 * `device` is the controller's handle, `interface` the interface number the
 * class requests are addressed to, and the two endpoint handles are the
 * controller's indexes for the bulk pair. Returns 0 when the stick was taken
 * and -1 when it was refused, which is recorded by name.
 */
int msc_attach(int device, int interface, int ep_in, int ep_out);

/**
 * Give up a device that has gone or that the core is closing.
 *
 * Every request in flight or waiting is completed with `EIO`, the block
 * device stops answering, and nothing of the stick is reused. Safe to call
 * for a device this driver never took.
 */
void msc_detach(int device);

/**
 * Step every stick by `elapsed_ms`.
 *
 * Called from the USB pump after the controller's completions have been
 * drained, so a completion and the transfer it allows are one pass apart at
 * most.
 */
void msc_step(uint32_t elapsed_ms);

/**
 * Hand this driver a completion that names one of its endpoints.
 *
 * Returns 1 when the completion belonged to a stick and was taken, 0 when it
 * did not.
 */
struct usb_completion;
int msc_completion(const struct usb_completion *completion);

/** How many sticks are ready to serve requests. */
int msc_count(void);

/**
 * The block device of the stick in slot `index`, or null when that slot has
 * none ready. The device layer publishes the nodes from these.
 */
struct block_device *msc_block_device(int index);

#endif /* _USB_MSC_H_ */
