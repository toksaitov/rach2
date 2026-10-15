/*
 * msc.h: the USB mass-storage class driver, over Bulk-Only Transport.
 *
 * A flash drive is a SCSI target reached through two bulk endpoints. Every
 * command is three transfers: a 31-byte command block wrapper out, the data
 * in or out when there is any, and a 13-byte status wrapper in. This driver
 * is a state machine over those three, stepped from the one pump the timer
 * and the controller's interrupt share, with one command in flight per
 * device.
 *
 * Nothing here waits. The block layer's sleeping side waits on the request,
 * not on this driver. The core's calls, from attach to the completions, run
 * in the pump with interrupts off; the disk layer's run in the pump or in a
 * process; and the block device's submit and flush run in the caller's
 * process, with interrupts off.
 */
#ifndef _USB_MSC_H_
#define _USB_MSC_H_

#include "../lib/kint.h"

/** How many sticks this kernel drives at once. */
#define MSC_MAX_DEVICES 8

/* How many logical units of one device are driven: a card reader has a
   slot per unit, a stick has one. */
#define MSC_MAX_LUNS 4

/**
 * What a stick says it is, for `/proc/usb` and the disk layer.
 *
 * The two names are SCSI's, from INQUIRY, trimmed of the spaces the standard
 * pads them with. `closing` is a stick that has gone whose record is still
 * held, by the disk layer or until its slot is given back.
 */
struct msc_ident {
    char vendor[9];
    char product[17];
    char serial[64]; /* the unit's serial page, or empty */
    uint64_t blocks; /* 512-byte blocks */
    uint32_t block_size;
    uint32_t flushes; /* SYNCHRONIZE CACHE commands sent to it */
    int write_protect;
    int closing;
    int device; /* the controller's slot */
};

/* A stick is on one controller; its transfers go to that one. */
struct xhci_ctrl;

/**
 * Take a device the core has enumerated as Bulk-Only mass storage.
 *
 * `device` is the controller's handle, `interface` the interface number the
 * class requests are addressed to, and the two endpoint handles are the
 * controller's indexes for the bulk pair. The stick is brought up by
 * `msc_step` from here. Returns 0 when the stick was taken and -1 when it was
 * refused, which is recorded by name.
 */
int msc_attach(struct xhci_ctrl *hc, int device, int interface, int ep_in, int ep_out);

/**
 * Give up a device that has gone or that the core is closing.
 *
 * Every request waiting is completed with `EIO`, and so is the one in flight
 * unless the controller may still reach its pages, in which case it is kept
 * until `msc_slot_closed`. The block device stops answering, the disk layer
 * is told (`usbdisk_left`), and nothing of the stick is reused until the
 * disk layer has let go (`msc_release`) and the slot has been given back.
 * Safe to call for a device this driver never took.
 */
void msc_detach(struct xhci_ctrl *hc, int device);

/** Disable Slot was answered with success for `device`: the controller
 * reaches nothing of the stick's any more, its request in flight is
 * completed and its record may go once the disk layer has let go. */
void msc_slot_closed(struct xhci_ctrl *hc, int device);

/**
 * Step every stick by `elapsed_ms`: send each one's next command, act on
 * each command that ended, run recovery, and charge every deadline.
 *
 * Called from the USB pump after the controller's completions have been
 * drained, so a completion and the transfer it allows are one pass apart at
 * most.
 */
void msc_step(uint32_t elapsed_ms);

struct usb_completion;

/**
 * Hand this driver a completion that names one of its endpoints.
 *
 * Returns 1 when the completion belonged to a stick and was taken, 0 when it
 * did not.
 */
int msc_completion(const struct usb_completion *completion);

/** How many sticks are ready to serve requests. */
int msc_count(void);

/** Whether entry `index` of this driver's table holds a stick that is
 * serving requests. */
int msc_ready(int index);

/** As `msc_lun_ident`, for logical unit 0. */
int msc_ident(int index, struct msc_ident *out);

/**
 * Fill `out` with what logical unit `lun` of the stick at entry `index` is:
 * INQUIRY's names, the unit's serial page, its capacity and write protection,
 * the stick's flushes and whether it is closing. Returns 0, or -1 for an
 * entry or a unit that is not there.
 */
int msc_lun_ident(int index, int lun, struct msc_ident *out);

/**
 * Free the entry of a stick that has gone and whose device nothing holds.
 *
 * The disk layer calls it and nothing else does: an entry freed while a
 * mount, a description or a held buffer still points at its block device
 * would hand those pointers to the next stick plugged in. The entry is freed
 * once its slot has been given back as well.
 */
void msc_release(int index);

/**
 * The block device of logical unit 0 of the stick at entry `index`, or NULL
 * when the entry is free or has no such unit. A device whose stick has gone
 * answers every request with `EIO`; the pointer stays valid until the entry
 * is released, because a mount and a held buffer still point at it.
 */
struct block_device *msc_block_device(int index);

/** The block device of logical unit `lun` of the stick at `index`, as
 * `msc_block_device`, or NULL. */
struct block_device *msc_lun_device(int index, int lun);

#endif /* _USB_MSC_H_ */
