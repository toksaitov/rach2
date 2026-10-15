/*
 * xhci.h: the xHCI host controller driver: the discovery and bring-up of each
 * controller, and its ports, commands, transfers, events and interrupts.
 *
 * xHCI is the only USB controller interface this kernel drives, and one xHCI
 * controller handles every device speed. `Readme.md` in this directory gives
 * the design. The callers are `usb.c`, which brings the controllers up and
 * runs the pump, the core and the class drivers, and the host tests, which
 * drive the code through a model of the controller (`xhci_set_io`).
 *
 * Bring-up runs once at boot, with interrupts on, and it and the stop at
 * shutdown are the only parts of the driver that wait. Every other call
 * returns at once: work is queued, and a later call finds it finished. The
 * pump makes those calls with interrupts off, from the timer tick and from the
 * controller's interrupt, and a caller in process context turns interrupts
 * off around its own.
 */

#ifndef _XHCI_H_
#define _XHCI_H_

#include "../lib/kint.h"
#include "hcd.h"

/*
 * The most scratchpad pages this driver gives a controller. The pages come
 * from the heap or the DMA pool, as many as the controller requires; the
 * limit is the one page that holds the array of their addresses, 512
 * eight-byte entries. The specification allows a controller to require 1023,
 * and one that requires more than 512 is not driven.
 */
#define XHCI_MAX_SCRATCHPADS 512

/*
 * How many controllers this kernel drives at once, the size of the table of
 * their records. On a machine with more, the first this many that the PCI
 * walk finds are driven and the rest are logged.
 */
#define XHCI_MAX_CONTROLLERS 4

/*
 * One controller. The record is the driver's, in a static table; a caller
 * gets one from `xhci_controller` and passes it to every call about that
 * controller. `xhci_forget` moves the records after the one it drops, so a
 * pointer is fixed only once bring-up is over.
 */
struct xhci_ctrl;

/** How many controllers the table has: those the last discovery took, less
 * those forgotten since. */
int xhci_controllers(void);

/** The controller at place `n` of the table, from zero; NULL past its end. */
struct xhci_ctrl *xhci_controller(int n);

/** The controller's place in the table, which its log lines give as
 * `xhciN`; -1 for NULL. */
int xhci_number(const struct xhci_ctrl *hc);

/**
 * Take a controller whose bring-up failed out of the table, and give back
 * the tables it took.
 *
 * A call about a port of such a controller could read through a null
 * pointer, since its port state may never have been taken. The controllers
 * after it move up one place and are renumbered, so the caller's walk of the
 * table steps back onto the one now in its place. Called only during
 * bring-up, which goes in table order, so no controller that moves has been
 * brought up. A pointer that is not in the table is ignored.
 */
void xhci_forget(struct xhci_ctrl *hc);

/*
 * The most device slots a controller may have, and how many transfer request
 * blocks each of its command and event rings holds. Slots are numbered from
 * one, so the tables that hold them have one more entry than the count. The
 * standard's field is a byte, so 255 is its whole range; what a controller is
 * told to enable is its own number, and its slot table, context array and
 * device contexts are sized from that rather than from a number compiled
 * here. Each of the two rings is one page, which is 256 blocks of sixteen
 * bytes.
 */
#define XHCI_SLOTS     255
#define XHCI_RING_TRBS 256

/*
 * The controller as this driver reaches it: the two register accessors and the
 * delay. A machine uses the mapped aperture and `k_udelay`; a host test
 * supplies a model of the controller instead, which is the only way to make a
 * reset that never completes, firmware that never answers, or a scratchpad
 * request no machine would make happen on demand.
 */
struct xhci_io {
    uint32_t (*r32)(struct xhci_ctrl *hc, uint32_t offset);
    void (*w32)(struct xhci_ctrl *hc, uint32_t offset, uint32_t value);
    void (*udelay)(uint32_t microseconds);
};

/** Use `ops` for the controllers discovered from now on, each of which keeps
 * the accessors it was discovered with; NULL restores the mapped registers
 * and `k_udelay`. For the host tests. */
void xhci_set_io(const struct xhci_io *ops);

/**
 * What the controller says about itself, read once by `xhci_read_caps`.
 *
 * Every field is read from the hardware rather than assumed: controllers
 * differ in the context size, the addressing width, the scratchpad count and
 * the register offsets, and a driver that assumed any of them would corrupt
 * memory on a controller that differed.
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

/* The boot line's `rach2.dma32`, a test switch the USB layer sets before
 * discovery: every controller is treated as one whose AC64 is clear, so its
 * pages come from the DMA pool and a buffer above 4 GiB moves through it. */
extern int xhci_dma32;

/**
 * Find the USB controllers on the PCI bus and take a record for each xHCI.
 *
 * Every EHCI controller is first taken from the firmware and halted, since on
 * an Intel chipset its pins are about to be routed to the xHCI. Then each
 * xHCI controller the walk finds, up to `XHCI_MAX_CONTROLLERS`, is put in
 * power state D0, has its Intel pin routing set and its memory space and bus
 * mastering enabled, and has the first 64 KiB of BAR 0 mapped. A controller
 * of another interface, one past the table's end, and one whose aperture
 * could not be mapped are logged and not driven: the i686 kernel cannot map
 * an aperture above 4 GiB, and the x86-64 kernel maps it where it is.
 * Discovery may run again, and fills the table from its start.
 *
 * Returns 0 when at least one controller was taken, or -1 with
 * `xhci_error(NULL)` saying why. Waits up to a second for each EHCI's
 * firmware and 10 ms for a function leaving D3.
 */
int xhci_discover(void);

/**
 * Take the controller from the firmware through its USB legacy support
 * capability.
 *
 * Follows the extended capability list from `HCCPARAMS1`, so it needs
 * `xhci_read_caps` first. On the capability it sets the OS-owned bit, waits
 * up to a second for the firmware to clear the BIOS-owned bit, and then turns
 * off the firmware's system-management interrupts and clears their status;
 * what the capability had before is kept for `xhci_release`. A controller
 * with no such capability is the operating system's already.
 *
 * Returns 0, or -1 with `xhci_error` saying why: the firmware kept the
 * controller, the controller did not take the OS-owned bit, or the list ran
 * past the mapped window. A controller the firmware keeps is left to it, with
 * any PS/2 emulation the firmware runs on it.
 */
int xhci_own(struct xhci_ctrl *hc);

/**
 * Halt the controller and reset it.
 *
 * Waits up to a second for Controller Not Ready to clear, clears Run/Stop and
 * waits up to 16 ms for the halt, sets the reset, pauses the millisecond an
 * Intel controller needs before its registers may be read again, and waits
 * up to a second for the reset to end and the controller to be ready.
 * Returns 0, or -1 with `xhci_error` saying which wait ran out.
 */
int xhci_halt_reset(struct xhci_ctrl *hc);

/**
 * Read the capability registers into `xhci_capabilities`, check them, and
 * take the tables sized from them.
 *
 * A controller is not driven whose registers describe no controller, whose
 * ports, doorbells or runtime registers lie outside the 64 KiB mapped window,
 * or that does not take 4 KiB pages: the first would have the driver write
 * into whatever follows the window, and the second would have it give the
 * controller pages of the wrong size. The tables are a record for each slot,
 * one page for the device context base address array, and the state of each
 * root port; those an earlier call took are given back first. Returns 0, or
 * -1 with `xhci_error` saying why. The first step of bring-up.
 */
int xhci_read_caps(struct xhci_ctrl *hc);

/**
 * Give the controller the scratchpad pages it requires.
 *
 * Takes a zeroed page for the array of their physical addresses and a zeroed
 * page for each, from the heap or, for a controller limited to 32-bit
 * addresses, the DMA pool; `xhci_start` puts the array's address in entry 0
 * of the device context base address array. A controller that requires more
 * than `XHCI_MAX_SCRATCHPADS` is not driven, and both numbers are logged.
 * Returns 0, or -1 with `xhci_error` saying why.
 */
int xhci_scratchpads(struct xhci_ctrl *hc);

/**
 * Program the controller's rings and start it.
 *
 * Clears the command and event rings, the device context base address array
 * and the driver's slot, port, command and completion state; enables every
 * slot the controller has; points the controller at the context array, the
 * command ring and a one-segment event ring on interrupter 0; sets Run/Stop
 * and waits up to 16 ms for the controller to leave the halted state; and
 * powers each root port that has no power. Interrupts stay off until
 * `xhci_interrupts_enable` or `xhci_interrupts_enable_line`. Returns 0, or -1
 * with `xhci_error` saying why.
 */
int xhci_start(struct xhci_ctrl *hc);

/**
 * Stop the controller before the machine halts or restarts.
 *
 * A running controller has Run/Stop and its interrupt enable cleared and is
 * given up to 16 ms to halt. It stays the operating system's, so that the
 * firmware does not take it back and start its legacy emulation during the
 * shutdown. A controller that is not running is given back to the firmware
 * with `xhci_release`. A controller left running can wake or restart a
 * machine that has shut down, which other operating systems work around
 * with a quirk for some Intel controllers.
 */
void xhci_stop(struct xhci_ctrl *hc);

/**
 * Give the controller back to the firmware.
 *
 * Restores the legacy support control register as `xhci_own` found it and
 * clears the OS-owned bit; does nothing when `xhci_own` did not take the
 * controller. Called for a controller whose bring-up failed, and by
 * `xhci_stop` for one that is not running: a kernel that took a controller
 * and cannot drive it does not keep it, since on a machine whose firmware
 * emulates a PS/2 keyboard from a USB one, what it kept would be the
 * keyboard.
 */
void xhci_release(struct xhci_ctrl *hc);

/** How many root ports the controller has; 0 for NULL. */
int xhci_ports(struct xhci_ctrl *hc);

/** What root port `port`, numbered from zero, reports now; -1 for a port the
 * controller does not have. */
int xhci_port_status(struct xhci_ctrl *hc, int port, struct usb_port_status *out);

/**
 * Whether root port `port` has been connected long enough to reset.
 *
 * A contact bounces, and the specification requires a hundred milliseconds
 * between a connect and a reset. The core calls this before it resets a root
 * port, so a device pushed in slowly is enumerated once rather than three
 * times. Returns 1 when the port has been connected for 100 ms by the
 * driver's clock, as the last `xhci_events` saw it, and 0 otherwise.
 */
int xhci_port_settled(struct xhci_ctrl *hc, int port);

/** Begin resetting root port `port`, which has 500 ms to finish. Returns -1
 * when the port is out of range or has no device. */
int xhci_port_reset_begin(struct xhci_ctrl *hc, int port);

/** 1 when the reset finished with the port enabled, 0 while it has not
 * finished, -1 when it finished with the port disabled, ran past its 500 ms,
 * or the port is out of range. A finished reset's change bit is cleared. */
int xhci_port_reset_done(struct xhci_ctrl *hc, int port);

/**
 * Put one command on the command ring and ring doorbell 0.
 *
 * `type` is the command's TRB type. `slot` has the slot the command acts on
 * in its low byte, zero for none, and for an endpoint command the endpoint's
 * device context index in bits 16 to 20, where the command block has it.
 * Returns 0 when the command was queued, -1 when one is in flight or an abort
 * has not ended. One command is in flight at a time, which is what lets this
 * driver hold one ring and no queue of its own; a command that has not ended
 * a second later is aborted.
 */
int xhci_command_issue(struct xhci_ctrl *hc, uint32_t type, uint64_t parameter, uint32_t slot);

/**
 * Report the command in flight: 1 when it succeeded, 0 while it has not
 * ended, -1 when it failed or there is no command to report.
 *
 * `out`, when not NULL, receives the result, which is given once. A command
 * past its deadline is aborted, and stays in flight until its own completion
 * or the Command Ring Stopped event ends it; a success in the meantime gives
 * its slot as usual, so the caller can close it. An abort that never ends
 * keeps this returning 0.
 */
int xhci_command_done(struct xhci_ctrl *hc, struct usb_command_result *out);

/**
 * Drain the event ring, scan the root ports, and return the events taken.
 *
 * A command completion ends the command in flight; a transfer event is
 * matched to its transfer, whose completion is queued for `xhci_poll`; a port
 * change marks the port changed. At most one ring's worth of events is taken
 * in a call, and a transfer event that finds the completion queue full stays
 * on the ring for the next call. Returns 0 on a controller that is not
 * running.
 */
int xhci_events(struct xhci_ctrl *hc);

/**
 * Have the controller send its interrupts as messages on `vector`.
 *
 * Programs the MSI capability first, then writes the moderation interval to
 * zero and sets the interrupter's and the controller's enables. Returns 0
 * when the controller will send messages, -1 when it is not running or its
 * MSI capability could not be enabled, in which case nothing was enabled.
 */
int xhci_interrupts_enable(struct xhci_ctrl *hc, uint8_t vector);

/** The IRQ of the controller's PCI line, 1 to 15, from its interrupt line
 * register; -1 when the controller is not running or has no line in that
 * range. */
int xhci_line_irq(struct xhci_ctrl *hc);

/**
 * Have the controller raise its interrupts on its PCI line instead.
 *
 * For a controller without messages: under `rach2.nomsi`, without a usable
 * local APIC, or when `xhci_interrupts_enable` failed. Clears the Interrupt
 * Disable bit of the PCI command register, writes the moderation interval to
 * zero and sets both enables; the caller registers the line's handler first.
 * Returns the line's IRQ, or -1 when the controller is not running or has no
 * line, in which case nothing was enabled.
 */
int xhci_interrupts_enable_line(struct xhci_ctrl *hc);

/** Stop the interrupts, by message or line; the controller keeps running and
 * the timer pumps it. Does nothing when neither is on. */
void xhci_interrupts_disable(struct xhci_ctrl *hc);

/**
 * Whether a command can be issued now: the controller is running, no command
 * is in flight, and any abort has had its stop event and seen CRR clear. The
 * core gives a device the controller's command ring only when this says so,
 * so a command it then issues is not turned away for a ring another device
 * left busy.
 */
int xhci_command_free(struct xhci_ctrl *hc);

/**
 * Log the controller's status, command ring and interrupter registers, the
 * driver's view of the command in flight, and the event ring's position.
 *
 * Once per boot, for whichever running controller calls first: the core calls
 * it when a device fails on a controller whose interrupts are on, so a boot
 * that lost its devices says whether the controller halted, stopped its ring,
 * or was never given the command.
 */
void xhci_state_record(struct xhci_ctrl *hc);

/** Whether the controller is raising its interrupts, by message or line. */
int xhci_interrupts_on(struct xhci_ctrl *hc);

/**
 * Acknowledge the interrupt at the controller, keeping the interrupter on.
 *
 * Clears `IMAN.IP` and leaves the enable beside it set: a bare write of the
 * pending bit would clear the enable too, and no later interrupt would be
 * raised. A controller on its line has `USBSTS.EINT` cleared first. Does
 * nothing on a controller that is not running. For the interrupt handler,
 * through `xhci_interrupt_take`.
 */
void xhci_interrupt_ack(struct xhci_ctrl *hc);

/**
 * Acknowledge the interrupt and drain the ring, in that order.
 *
 * What a handler calls. An event written between a drain and an
 * acknowledgement that came after it would be left unread with its pending
 * bit cleared, which is why the two are one call. Returns what was drained.
 */
int xhci_interrupt_take(struct xhci_ctrl *hc);

/**
 * Advance the controller's clock by `milliseconds`, and abort a command past
 * its deadline.
 *
 * The clock times the port debounce, the port reset and the command
 * deadline. The pump calls this once a pass, with the time since the last:
 * 10 ms from the timer and none from an interrupt.
 */
void xhci_advance(struct xhci_ctrl *hc, uint32_t milliseconds);

/**
 * Ask the controller for a slot for the device at `where`, on a reset port,
 * with an Enable Slot command.
 *
 * `xhci_command_done` reports the slot, which is the device handle every
 * later call takes; the driver records `where` and `speed` in it when the
 * command completes, since the controller chooses the slot and does not say
 * which until then. Returns 0 when the command was issued, and -1 when it was
 * not or when `where` has a root port the controller lacks, a depth past five
 * tiers, route bits above its depth, or a translator outside the slots and
 * the 255 ports there can be.
 */
int xhci_device_open(struct xhci_ctrl *hc, const struct usb_location *where, int speed);

/**
 * Give a device its address and its default control endpoint, with an
 * Address Device command.
 *
 * Takes the slot's device context and endpoint zero's ring, a page each, if
 * the slot has none yet, and fills the input context with the slot's place in
 * the tree and with endpoint zero, whose packet size is 8, 64 or 512 by the
 * device's speed. Returns 0 when the command was issued; -1 for a slot not in
 * use, while a command is busy, or when a page could not be had.
 */
int xhci_device_address(struct xhci_ctrl *hc, int device);

/**
 * Tell the controller that a device is a hub, with an Evaluate Context
 * command: the hub bit and its port count, and for a high-speed hub whether
 * it translates per port and its think time, all in the hub's own slot
 * context. Returns 0 when the command was issued; -1 for a slot not in use,
 * while a command is busy, or for a port count outside 1 to 255 or a think
 * time outside 0 to 3.
 */
int xhci_device_hub(struct xhci_ctrl *hc, int device, int ports, int multi_tt, int think_time);

/**
 * Tell the controller the real size of a device's control endpoint, with an
 * Evaluate Context command.
 *
 * A full-speed device does not say what its control endpoint takes until it
 * has been read, and reading it needs a control endpoint. Eight bytes is a
 * size every device takes; this corrects it once the descriptor has arrived.
 * Returns 0 when the command was issued; -1 for a slot not in use or while a
 * command is busy.
 */
int xhci_device_packet_size(struct xhci_ctrl *hc, int device, int max_packet);

/**
 * Ask the controller for a device's slot back, with a Disable Slot command.
 *
 * The endpoints stop taking transfers at once; the slot, its context and its
 * rings stay the controller's until the command completes, and the caller
 * then calls `xhci_device_closed`. Returns -1 for a slot not in use or when
 * the command could not be issued, and the caller may try again.
 */
int xhci_device_close(struct xhci_ctrl *hc, int device);

/** Finish a close once the Disable Slot has completed: the slot is free, its
 * entry in the context array is cleared, and its pages and transfer records
 * are given back. */
void xhci_device_closed(struct xhci_ctrl *hc, int device);

/** Whether a command is in flight or an abort has not ended. */
int xhci_command_busy(struct xhci_ctrl *hc);

/**
 * Add an interrupt or bulk endpoint to a device's configuration, with a
 * Configure Endpoint command.
 *
 * The endpoint's handle is its device context index, which the caller works
 * out; endpoint zero is opened by `xhci_device_address`. The ring, a page,
 * and the endpoint's two transfer records are taken on its first open.
 * Returns 0 when the command was issued; -1 for a slot not in use, an
 * endpoint number above 15, while a command is busy, or when memory could not
 * be had.
 */
int xhci_endpoint_open(struct xhci_ctrl *hc, int device, const struct usb_endpoint *endpoint);

/* An endpoint's state as the controller last reported it, which decides how
 * `usb_core_endpoint_quiesce` stops it: Stop Endpoint on a running one, Reset
 * Endpoint on a halted one, and neither on a stopped one or one in the Error
 * state; a Set TR Dequeue follows each. */
#define XHCI_EP_RUNNING 0
#define XHCI_EP_HALTED  1
#define XHCI_EP_STOPPED 2
/* A block the controller could not execute, a TRB Error or a data buffer
 * error: the endpoint is in the Error state, which a Set TR Dequeue alone
 * moves to Stopped (xHCI 1.2, 4.8.3, 4.6.10). */
#define XHCI_EP_ERROR   3

/** Reset a halted endpoint with a Reset Endpoint command; a Set TR Dequeue
 * (`xhci_endpoint_dequeue_here`) follows once it has completed, and
 * `usb_core_endpoint_quiesce` issues the two. Returns 0 when the command was
 * issued, -1 otherwise. */
int xhci_endpoint_reset(struct xhci_ctrl *hc, int device, int endpoint);

/**
 * Queue one transfer on endpoint handle `endpoint` of `device` and ring its
 * doorbell.
 *
 * Its end is reported once, by `xhci_poll`. A control transfer's data stage
 * is given to the controller as one piece from the buffer's first byte, so
 * its buffer must be physically contiguous. Returns 0 when the transfer was
 * queued, and -1, with nothing given to the controller, when the endpoint is
 * not open, is being quiesced, or is halted or in the Error state; when a
 * page of the buffer has no frame, or the buffer needs more blocks than a
 * record holds or than the ring has free; or when both of the endpoint's
 * records are in use or a page of the DMA pool could not be had.
 */
int xhci_submit(struct xhci_ctrl *hc, int device, int endpoint,
                const struct usb_transfer *transfer);

/** Take the oldest transfer completion into `out`: 1 when there was one, 0
 * when none was waiting. */
int xhci_poll(struct xhci_ctrl *hc, struct usb_completion *out);

/** Whether the transfer submitted under `generation` on `device` is retired,
 * so no block of it can be fetched again: 1 or 0, or -1 when the driver has
 * no record of that transfer. */
int xhci_td_retired(struct xhci_ctrl *hc, int device, uint32_t generation);

/** Hold the record of the transfer just submitted under `generation`, so
 * it answers `xhci_td_retired` after its events until it is let go; a
 * record not held is reused once it is retired. -1 for no such transfer. */
int xhci_td_hold(struct xhci_ctrl *hc, int device, uint32_t generation);

/** The caller is done with the transfer: its record may be reused once it
 * is retired. */
void xhci_td_release(struct xhci_ctrl *hc, int device, uint32_t generation);

/** The endpoint's state as the controller last reported it, one of the
 * `XHCI_EP_` values; -1 for a slot not in use or no such endpoint. */
int xhci_endpoint_state(struct xhci_ctrl *hc, int device, int endpoint);

/** Set or clear the endpoint's quiescing mark; while it is set, nothing is
 * submitted on the endpoint. The core sets it while it stops the endpoint. */
void xhci_endpoint_quiescing(struct xhci_ctrl *hc, int device, int endpoint, int on);

/** Stop a running endpoint with a Stop Endpoint command. Returns 0 when the
 * command was issued, -1 otherwise. */
int xhci_endpoint_stop(struct xhci_ctrl *hc, int device, int endpoint);

/** Set TR Dequeue to the endpoint's enqueue pointer with its cycle, leaving
 * the driver's own pointer as it is. Returns 0 when the command was issued,
 * -1 otherwise. */
int xhci_endpoint_dequeue_here(struct xhci_ctrl *hc, int device, int endpoint);

/** Retire every transfer of the endpoint and mark it stopped, once the
 * commands that stopped or reset it and set its dequeue have completed with
 * success. */
void xhci_endpoint_retire_all(struct xhci_ctrl *hc, int device, int endpoint);

/** How many transfer events for `device` matched no outstanding transfer
 * and were dropped. */
uint32_t xhci_dropped_events(struct xhci_ctrl *hc, int device);

/** What was read from the controller, valid after `xhci_read_caps`. */
const struct xhci_caps *xhci_capabilities(struct xhci_ctrl *hc);

/** Why the controller's last bring-up step failed, or "none"; for NULL, why
 * the last discovery took no controller. */
const char *xhci_error(struct xhci_ctrl *hc);

#endif
