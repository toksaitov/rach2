#ifndef _XHCI_H_
#define _XHCI_H_

#include "../lib/kint.h"
#include "hcd.h"

/**
 * The xHCI host controller driver: the only USB controller this kernel drives.
 *
 * xHCI is the one register interface both machines have, and the only one that
 * reaches a low-speed keyboard without a hub driver and split transactions.
 * `sys/usb/Readme.md` gives the reasoning and the layer boundaries; this header
 * is the part of the driver the rest of the kernel and its host test reach.
 *
 * Bring-up runs once at boot, in thread context, and is the only place in this
 * driver that waits. Everything after it is queued work and polled completion,
 * because the events are drained from the timer handler.
 */

/*
 * How many scratchpad pages this kernel will hand a controller. The
 * specification allows a controller to ask for as many as 1023, which is four
 * megabytes on a machine limited to thirty-two, so the pages are compiled in
 * and a controller that asks for more is refused by name rather than served
 * badly. No controller met so far asks for anything near this.
 */
#define XHCI_MAX_SCRATCHPADS 32

/*
 * How many device slots this kernel asks the controller to enable, and how
 * many transfer request blocks each of its two rings holds. Slots are numbered
 * from one, so the tables that hold them have one more entry than this and
 * their first is never used. A controller may
 * offer 64 slots or more; four is what the fixed tables above this driver
 * hold, and asking for more would have the controller reserve memory for
 * devices that could never be opened. Each ring is one page, which is 256
 * blocks of sixteen bytes.
 */
#define XHCI_SLOTS     8
#define XHCI_RING_TRBS 256

/*
 * The controller as this driver reaches it: the two register accessors and the
 * delay. A machine uses the mapped aperture and `k_udelay`; a host test
 * supplies a model of the controller instead, which is the only way to make a
 * reset that never completes, firmware that never answers, or a scratchpad
 * request no machine would make happen on demand.
 */
struct xhci_io {
    uint32_t (*r32)(uint32_t offset);
    void (*w32)(uint32_t offset, uint32_t value);
    void (*udelay)(uint32_t microseconds);
};

/** Use `ops` instead of the mapped controller; NULL restores the machine's. */
void xhci_set_io(const struct xhci_io *ops);

/**
 * What the controller says about itself, read once and not read again.
 *
 * Every field here is read from the hardware rather than assumed, because the
 * two machines this kernel runs on differ in several of them: the context
 * size, the addressing width, the scratchpad count and the register offsets
 * are all controller properties, and a driver that guessed any of them would
 * work on one machine and corrupt memory on the other.
 */
struct xhci_caps {
    uint32_t caplength;    /* where the operational registers begin */
    uint32_t hciversion;   /* the interface version, in binary-coded decimal */
    uint32_t max_slots;    /* devices the controller can address at once */
    uint32_t max_intrs;    /* interrupters; this driver uses the first alone */
    uint32_t max_ports;    /* root ports */
    uint32_t erst_max;     /* event-ring segment table entries, as a power of two */
    uint32_t scratchpads;  /* pages the controller requires for its own use */
    uint32_t context_size; /* 32 or 64 bytes, from `HCCPARAMS1.CSZ` */
    int ac64;              /* whether the controller takes 64-bit addresses */
    uint32_t xecp;         /* byte offset of the extended capability list, 0 if none */
    uint32_t dboff;        /* byte offset of the doorbell array */
    uint32_t rtsoff;       /* byte offset of the runtime registers */
    uint32_t pagesize;     /* the page sizes the controller supports, as a bitmap */
};

/**
 * Find an xHCI controller, put it in D0, and map its register aperture.
 *
 * Returns 0 when one was found and mapped, or -1 with `xhci_error` set. A
 * 64-bit aperture above 4 GiB is refused rather than truncated: this kernel
 * addresses 32 bits and a truncated base would read some other device's
 * registers.
 */
int xhci_discover(void);

/**
 * Take the controller from the firmware.
 *
 * Walks the extended capability list for the legacy-support capability, asks
 * for ownership, waits for the firmware to give it up, and then turns off the
 * system-management interrupts it was using. A controller with no such
 * capability is already the operating system's and this returns 0 at once.
 * Returns -1 when the firmware never answers, which leaves the machine's
 * firmware-emulated PS/2 input working rather than racing it.
 */
int xhci_own(void);

/**
 * Halt the controller and reset it.
 *
 * Stops the schedule, waits for the halt, asserts the reset, waits the fixed
 * millisecond Intel controllers require before their registers may be read
 * again, and then waits for the reset to finish and for the controller to
 * report that it is ready. Returns -1 if any of those never happens.
 */
int xhci_halt_reset(void);

/**
 * Read the capability registers and check that they fit in the mapped window.
 *
 * A controller whose doorbells or runtime registers lie outside the aperture
 * this driver mapped, or that does not support 4 KiB pages, is refused: the
 * first would have the driver write into whatever follows the window, and the
 * second would have it hand the controller pages of the wrong size.
 */
int xhci_read_caps(void);

/**
 * Give the controller the scratchpad pages it asked for.
 *
 * Fills the scratchpad pointer array with zeroed, page-aligned pages. The
 * array is pointed at by the first entry of the device-context base address
 * array, which bring-up programs later. A controller that requires more pages
 * than `XHCI_MAX_SCRATCHPADS` is refused with both numbers recorded.
 */
int xhci_scratchpads(void);

/**
 * Program the controller's rings and start it.
 *
 * Points the controller at the device-context base address array, the command
 * ring and the first interrupter's event ring, tells it how many slots this
 * kernel will use, and sets it running. Returns -1 if it does not leave the
 * halted state.
 */
int xhci_start(void);

/**
 * Stop the controller.
 *
 * Called before the machine halts or restarts. A controller left running can
 * wake or reset the machine it was in, which is what other operating systems
 * carry a quirk for on this exact Intel part.
 */
void xhci_stop(void);

/**
 * Give the controller back to the firmware.
 *
 * Restores the legacy-support registers to what they held before this kernel
 * asked for the controller. Called when bring-up fails after the handoff and
 * when the machine stops: a kernel that took a controller and cannot drive it
 * must not keep it, because on a machine whose firmware emulates a PS/2
 * keyboard out of a USB one, what it kept is the keyboard.
 */
void xhci_release(void);

/** How many root ports the controller has. */
int xhci_ports(void);

/** What a root port reports now. */
int xhci_port_status(int port, struct usb_port_status *out);

/**
 * Whether a port's connect has been steady long enough to act on.
 *
 * A contact bounces, and the specification asks for a hundred milliseconds of
 * quiet between a connect and a reset. The layer above asks this before it
 * resets a port, so a device pushed in slowly is enumerated once rather than
 * three times.
 */
int xhci_port_settled(int port);

/** Begin resetting a port. Returns -1 if the port has no device. */
int xhci_port_reset_begin(int port);

/** 1 when the reset finished, 0 while it has not, -1 when it gave up. */
int xhci_port_reset_done(int port);

/**
 * Issue one command.
 *
 * `type` is the command's transfer-block type and `slot` the slot it acts on,
 * zero where the command names none. Returns 0 when the command was queued,
 * -1 when one is already in flight or the ring is not usable. One command is
 * in flight at a time, which is what lets this driver hold one ring and no
 * queue of its own.
 */
int xhci_command_issue(uint32_t type, uint64_t parameter, uint32_t slot);

/**
 * Report the command in flight: 1 finished, 0 still running, -1 given up on.
 *
 * A command that passes its deadline is aborted, which is the only way to get
 * a command ring back from a controller that stopped answering.
 */
int xhci_command_done(struct usb_command_result *out);

/**
 * Drain the event ring.
 *
 * Called from the timer, so it takes what is there and returns. Command
 * completions and port changes are recorded for the pollers above; transfer
 * events belong to the next phase of this work.
 */
int xhci_events(void);

/**
 * Have the controller send its interrupts as messages on `vector`.
 *
 * The moderation interval is written to zero, both enables are set and the
 * MSI capability is programmed last. 0 when the controller will send them,
 * -1 when it will not, in which case nothing was enabled.
 */
int xhci_interrupts_enable(uint8_t vector);

/** Stop the messages; the controller keeps running and the timer pumps it. */
void xhci_interrupts_disable(void);

/** Whether the controller is sending its interrupts as messages. */
int xhci_interrupts_on(void);

/**
 * Acknowledge the interrupt at the controller, keeping the interrupter on.
 *
 * The handler's first act. A bare write of the pending bit would clear the
 * enable beside it, and no later interrupt would be raised.
 */
void xhci_interrupt_ack(void);

/**
 * Acknowledge the interrupt and drain the ring, in that order.
 *
 * What a handler calls. An event written between a drain and an
 * acknowledgement that came after it would be left unread with its pending
 * bit cleared, which is why the two are one call. Returns what was drained.
 */
int xhci_interrupt_take(void);

/** Advance the driver's own clock, in milliseconds. */
void xhci_advance(uint32_t milliseconds);

/**
 * Ask the controller for a slot for a device on a reset port.
 *
 * Issues one command; `xhci_command_done` reports the slot, which is the
 * device handle everything below uses. The port and speed are remembered
 * against the command, because which slot it will be is the controller's to
 * decide and it does not say until the command answers.
 */
int xhci_device_open(int port, int speed);

/** Address a device and give it its default control endpoint. */
int xhci_device_address(int device);

/**
 * Tell the controller the real size of a device's control endpoint.
 *
 * A full-speed device does not say what its control endpoint takes until it
 * has been read, and reading it needs a control endpoint. Eight bytes is the
 * size every device answers to; this corrects it once the descriptor has
 * arrived.
 */
int xhci_device_packet_size(int device, int max_packet);

/**
 * Ask the controller for a device's slot back.
 *
 * The endpoints stop taking work at once; the slot and its context stay the
 * controller's until the command completes, which is what `xhci_device_closed`
 * is called for. Returns -1 when the command could not be issued, and the
 * caller may try again.
 */
int xhci_device_close(int device);

/** Finish a close: the controller has given the slot back. */
void xhci_device_closed(int device);

/** Whether a command is in flight or being taken back. */
int xhci_command_busy(void);

/** Open an endpoint on a device. Returns its handle, which is its index. */
int xhci_endpoint_open(int device, const struct usb_endpoint *endpoint);

/** Forget an endpoint. */
void xhci_endpoint_close(int device, int endpoint);

/** Clear a halted endpoint. `xhci_endpoint_dequeue` follows it. */
int xhci_endpoint_reset(int device, int endpoint);

/** Point a reset endpoint's ring at the block this driver will write next. */
int xhci_endpoint_dequeue(int device, int endpoint);

/** Queue one transfer on an endpoint. */
int xhci_submit(int device, int endpoint, const struct usb_transfer *transfer);

/** 1 when a completion was waiting, 0 when none was. */
int xhci_poll(struct usb_completion *out);

/** What was read from the controller, valid after `xhci_read_caps`. */
const struct xhci_caps *xhci_capabilities(void);

/** Why the last step refused, for the record that reports it. */
const char *xhci_error(void);

#endif
