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
 * How many scratchpad pages this kernel will hand a controller. The pages
 * themselves come from the heap, as many as the controller asks for; this is
 * the limit of the one page that holds the array of their addresses, which is
 * 512 eight-byte entries. The specification allows a controller to ask for
 * 1023, and one that asks for more than fits the array is refused by name. No
 * controller met so far asks for anything near this.
 */
#define XHCI_MAX_SCRATCHPADS 512

/*
 * How many controllers this kernel drives at once. A machine with more than
 * this many says so and drives the first of them; two is what the notebooks
 * here have, and a fixed number keeps the records in one table.
 */
#define XHCI_MAX_CONTROLLERS 4

/*
 * One controller, as the rest of the kernel holds it. The record itself is
 * the driver's; a caller takes one from `xhci_controller` and gives it back
 * to every call it makes about that controller.
 */
struct xhci_ctrl;

/** How many controllers the last discovery found, and the nth of them. */
int xhci_controllers(void);
struct xhci_ctrl *xhci_controller(int n);

/** Which of them this is, or -1 for none; its place in the table. */
int xhci_number(const struct xhci_ctrl *hc);

/**
 * Drop a controller whose bring-up failed, so nothing asks about it.
 *
 * Its tables were never taken, and a port asked about on it would read
 * through a null. Called only while the controllers are being brought up,
 * before anything holds one.
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
int xhci_own(struct xhci_ctrl *hc);

/**
 * Halt the controller and reset it.
 *
 * Stops the schedule, waits for the halt, asserts the reset, waits the fixed
 * millisecond Intel controllers require before their registers may be read
 * again, and then waits for the reset to finish and for the controller to
 * report that it is ready. Returns -1 if any of those never happens.
 */
int xhci_halt_reset(struct xhci_ctrl *hc);

/**
 * Read the capability registers and check that they fit in the mapped window.
 *
 * A controller whose doorbells or runtime registers lie outside the aperture
 * this driver mapped, or that does not support 4 KiB pages, is refused: the
 * first would have the driver write into whatever follows the window, and the
 * second would have it hand the controller pages of the wrong size.
 */
int xhci_read_caps(struct xhci_ctrl *hc);

/**
 * Give the controller the scratchpad pages it asked for.
 *
 * Fills the scratchpad pointer array with zeroed, page-aligned pages. The
 * array is pointed at by the first entry of the device-context base address
 * array, which bring-up programs later. A controller that requires more pages
 * than `XHCI_MAX_SCRATCHPADS` is refused with both numbers recorded.
 */
int xhci_scratchpads(struct xhci_ctrl *hc);

/**
 * Program the controller's rings and start it.
 *
 * Points the controller at the device-context base address array, the command
 * ring and the first interrupter's event ring, tells it how many slots this
 * kernel will use, and sets it running. Returns -1 if it does not leave the
 * halted state.
 */
int xhci_start(struct xhci_ctrl *hc);

/**
 * Stop the controller.
 *
 * Called before the machine halts or restarts. A controller left running can
 * wake or reset the machine it was in, which is what other operating systems
 * carry a quirk for on this exact Intel part.
 */
void xhci_stop(struct xhci_ctrl *hc);

/**
 * Give the controller back to the firmware.
 *
 * Restores the legacy-support registers to what they held before this kernel
 * asked for the controller. Called when bring-up fails after the handoff and
 * when the machine stops: a kernel that took a controller and cannot drive it
 * must not keep it, because on a machine whose firmware emulates a PS/2
 * keyboard out of a USB one, what it kept is the keyboard.
 */
void xhci_release(struct xhci_ctrl *hc);

/** How many root ports the controller has. */
int xhci_ports(struct xhci_ctrl *hc);

/** What a root port reports now. */
int xhci_port_status(struct xhci_ctrl *hc, int port, struct usb_port_status *out);

/**
 * Whether a port's connect has been steady long enough to act on.
 *
 * A contact bounces, and the specification asks for a hundred milliseconds of
 * quiet between a connect and a reset. The layer above asks this before it
 * resets a port, so a device pushed in slowly is enumerated once rather than
 * three times.
 */
int xhci_port_settled(struct xhci_ctrl *hc, int port);

/** Begin resetting a port. Returns -1 if the port has no device. */
int xhci_port_reset_begin(struct xhci_ctrl *hc, int port);

/** 1 when the reset finished, 0 while it has not, -1 when it gave up. */
int xhci_port_reset_done(struct xhci_ctrl *hc, int port);

/**
 * Issue one command.
 *
 * `type` is the command's transfer-block type and `slot` the slot it acts on,
 * zero where the command names none. Returns 0 when the command was queued,
 * -1 when one is already in flight or the ring is not usable. One command is
 * in flight at a time, which is what lets this driver hold one ring and no
 * queue of its own.
 */
int xhci_command_issue(struct xhci_ctrl *hc, uint32_t type, uint64_t parameter, uint32_t slot);

/**
 * Report the command in flight: 1 finished, 0 still running, -1 given up on.
 *
 * A command that passes its deadline is aborted, which is the only way to get
 * a command ring back from a controller that stopped answering.
 */
int xhci_command_done(struct xhci_ctrl *hc, struct usb_command_result *out);

/**
 * Drain the event ring.
 *
 * Called from the timer, so it takes what is there and returns. Command
 * completions and port changes are recorded for the pollers above; transfer
 * events belong to the next phase of this work.
 */
int xhci_events(struct xhci_ctrl *hc);

/**
 * Have the controller send its interrupts as messages on `vector`.
 *
 * The moderation interval is written to zero, both enables are set and the
 * MSI capability is programmed last. 0 when the controller will send them,
 * -1 when it will not, in which case nothing was enabled.
 */
int xhci_interrupts_enable(struct xhci_ctrl *hc, uint8_t vector);

/** The IRQ of the controller's PCI line, or -1 when it names none. */
int xhci_line_irq(struct xhci_ctrl *hc);

/**
 * Have the controller raise its interrupts on its PCI line instead.
 *
 * The legacy path, for a machine with `rach2.nomsi` or without a usable
 * APIC. Returns the line's IRQ, or -1 when the controller names none, in
 * which case nothing was enabled.
 */
int xhci_interrupts_enable_line(struct xhci_ctrl *hc);

/** Stop the interrupts; the controller keeps running and the timer pumps it. */
void xhci_interrupts_disable(struct xhci_ctrl *hc);

/**
 * Whether a command can be issued now: none is running and no abort is
 * still waiting for the controller to stop the ring. The core takes the
 * enumeration turn only when this says so, so a command it then issues is
 * never refused for a ring another device left busy.
 */
int xhci_command_free(struct xhci_ctrl *hc);

/** Record the controller's registers and the command in flight, once. */
void xhci_state_record(struct xhci_ctrl *hc);

/** Whether the controller is raising its interrupts, by message or line. */
int xhci_interrupts_on(struct xhci_ctrl *hc);

/**
 * Acknowledge the interrupt at the controller, keeping the interrupter on.
 *
 * The handler's first act. A bare write of the pending bit would clear the
 * enable beside it, and no later interrupt would be raised.
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

/** Advance the driver's own clock, in milliseconds. */
void xhci_advance(struct xhci_ctrl *hc, uint32_t milliseconds);

/**
 * Ask the controller for a slot for a device at `where`, on a reset port.
 *
 * Issues one command; `xhci_command_done` reports the slot, which is the
 * device handle everything below uses. The location and speed are remembered
 * against the command, because which slot it will be is the controller's to
 * decide and it does not say until the command answers. A location deeper
 * than five tiers, or whose route has bits above its depth, is refused.
 */
int xhci_device_open(struct xhci_ctrl *hc, const struct usb_location *where, int speed);

/** Address a device and give it its default control endpoint. */
int xhci_device_address(struct xhci_ctrl *hc, int device);

/**
 * Tell the controller that a device is a hub: the hub bit and its port
 * count, and for a high-speed hub whether it translates per port and its
 * think time, all in the hub's own slot context, through the evaluate
 * command. Issues one command.
 */
int xhci_device_hub(struct xhci_ctrl *hc, int device, int ports, int multi_tt, int think_time);

/**
 * Tell the controller the real size of a device's control endpoint.
 *
 * A full-speed device does not say what its control endpoint takes until it
 * has been read, and reading it needs a control endpoint. Eight bytes is the
 * size every device answers to; this corrects it once the descriptor has
 * arrived.
 */
int xhci_device_packet_size(struct xhci_ctrl *hc, int device, int max_packet);

/**
 * Ask the controller for a device's slot back.
 *
 * The endpoints stop taking work at once; the slot and its context stay the
 * controller's until the command completes, which is what `xhci_device_closed`
 * is called for. Returns -1 when the command could not be issued, and the
 * caller may try again.
 */
int xhci_device_close(struct xhci_ctrl *hc, int device);

/** Finish a close: the controller has given the slot back. */
void xhci_device_closed(struct xhci_ctrl *hc, int device);

/** Whether a command is in flight or being taken back. */
int xhci_command_busy(struct xhci_ctrl *hc);

/** Open an endpoint on a device. Returns its handle, which is its index. */
int xhci_endpoint_open(struct xhci_ctrl *hc, int device, const struct usb_endpoint *endpoint);

/** Forget an endpoint. */
void xhci_endpoint_close(struct xhci_ctrl *hc, int device, int endpoint);

/* An endpoint's state as the controller last reported it, which decides
 * the command that stops it: Stop Endpoint on a running one, Reset Endpoint
 * on a halted one, none on one already stopped. */
#define XHCI_EP_RUNNING 0
#define XHCI_EP_HALTED  1
#define XHCI_EP_STOPPED 2
/* A block the controller could not execute, a TRB Error or a data buffer
 * error: the endpoint is in the Error state, which a Set TR Dequeue alone
 * moves to Stopped (xHCI 1.2, 4.8.3, 4.6.10). */
#define XHCI_EP_ERROR   3

/** Reset a halted endpoint, a command; `xhci_endpoint_dequeue_here` follows
 * it once it has answered. The core's `usb_core_endpoint_quiesce` runs the
 * two. */
int xhci_endpoint_reset(struct xhci_ctrl *hc, int device, int endpoint);


/** Queue one transfer on an endpoint. */
int xhci_submit(struct xhci_ctrl *hc, int device, int endpoint,
                const struct usb_transfer *transfer);

/** 1 when a completion was waiting, 0 when none was. */
int xhci_poll(struct xhci_ctrl *hc, struct usb_completion *out);

/** Whether the transfer submitted under `generation` is retired, so no block
 * of it can be fetched again: 1, 0, or -1 for a transfer the driver does
 * not hold, which a caller treats as not retired. */
int xhci_td_retired(struct xhci_ctrl *hc, int device, uint32_t generation);

/** Hold the record of the transfer just submitted under `generation`, so
 * it answers `xhci_td_retired` after its events until it is let go; a
 * record not held is reused once it is retired. -1 for no such transfer. */
int xhci_td_hold(struct xhci_ctrl *hc, int device, uint32_t generation);

/** The caller is done with the transfer: its record may be reused once it
 * is retired. */
void xhci_td_release(struct xhci_ctrl *hc, int device, uint32_t generation);

/** The endpoint's state as the controller last reported it: 0 running, 1
 * halted by a failure, 2 stopped; -1 for no such endpoint. */
int xhci_endpoint_state(struct xhci_ctrl *hc, int device, int endpoint);

/** While on, nothing is submitted on the endpoint: the core is stopping it. */
void xhci_endpoint_quiescing(struct xhci_ctrl *hc, int device, int endpoint, int on);

/** Stop Endpoint, a command; the core awaits it. */
int xhci_endpoint_stop(struct xhci_ctrl *hc, int device, int endpoint);

/** Set TR Dequeue to the endpoint's enqueue pointer with its cycle, leaving
 * the driver's own pointer as it is; the core awaits it. */
int xhci_endpoint_dequeue_here(struct xhci_ctrl *hc, int device, int endpoint);

/** After a stop or a reset and its dequeue were answered with success:
 * every transfer of the endpoint is retired. */
void xhci_endpoint_retire_all(struct xhci_ctrl *hc, int device, int endpoint);

/** Transfer events that named no transfer the driver held. */
uint32_t xhci_dropped_events(struct xhci_ctrl *hc, int device);

/** What was read from the controller, valid after `xhci_read_caps`. */
const struct xhci_caps *xhci_capabilities(struct xhci_ctrl *hc);

/** Why the last step refused, for the record that reports it. */
const char *xhci_error(struct xhci_ctrl *hc);

#endif
