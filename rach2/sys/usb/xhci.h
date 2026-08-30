#ifndef _XHCI_H_
#define _XHCI_H_

#include "../lib/kint.h"

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
    uint32_t scratchpads;  /* pages the controller wants for its own use */
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
 * array, which bring-up programs later. A controller that wants more pages
 * than `XHCI_MAX_SCRATCHPADS` is refused with both numbers recorded.
 */
int xhci_scratchpads(void);

/** What was read from the controller, valid after `xhci_read_caps`. */
const struct xhci_caps *xhci_capabilities(void);

/** Why the last step refused, for the record that reports it. */
const char *xhci_error(void);

#endif
