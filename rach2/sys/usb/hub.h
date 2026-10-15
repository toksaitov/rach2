/*
 * hub.h: the hub class driver.
 *
 * A hub is a device whose ports the core enumerates like the root
 * controller's. The core hands a configured hub here; this driver reads its
 * descriptor, powers its ports, watches its change endpoint, and answers the
 * core's port operations for the devices below it. Every call runs in the
 * USB pump, with interrupts off.
 */
#ifndef _USB_HUB_H_
#define _USB_HUB_H_

#include "../lib/kint.h"

/* How many hubs this kernel drives at once. A hub's record is a frame taken
 * when it attaches and given back when it goes, so this bounds the table of
 * records and not the memory they take. */
#define HUB_MAX 8

struct usb_port_status;
struct usb_completion;

/* A hub is on one controller, and its ports are reached through transfers to
 * the hub itself; the port operations take the controller to match the shape
 * a root port's take. */
struct xhci_ctrl;

/**
 * Take a hub the core has configured. `index` is the hub's device index in
 * the core, `device` the controller's handle, `interface` the interface
 * number, `protocol` the interface's protocol byte, `ep_dci` the change
 * endpoint's handle, `depth` the hub's tier below its root port, and `super`
 * whether it is a SuperSpeed hub. The request for its descriptor goes out
 * here, and `hub_step` does the rest. Returns 0 when the hub was taken, -1
 * when it was refused, with the reason in the record.
 */
int hub_attach(struct xhci_ctrl *hc, int index, int device, int interface, int protocol, int ep_dci,
               int depth, int super);

/**
 * Give up the hub at core index `index`, which has gone or which the core is
 * closing. Its record is kept until the core's entry is free, since the
 * controller may write into its buffers until its slot is given back.
 */
void hub_detach(int index);

/** Step every hub by `elapsed_ms`, from the pump, and give back the record of
 * each detached hub whose core entry is free. */
void hub_step(uint32_t elapsed_ms);

/**
 * Take a completion that names a hub's change endpoint, or its control
 * endpoint while a request is in flight; 1 when it did. A completion for a
 * transfer given up on is taken and dropped.
 */
int hub_completion(const struct usb_completion *completion);

/*
 * The port operations a device below a hub answers to, with `owner` the
 * hub's device index and `port` numbered from zero; `hc` is not used, since
 * a hub is reached through its own device. `hub_port_status` fills `out` from
 * the last status read and returns 0, or -1 for a port that is not there or
 * not yet read; a port powered off, waiting to be powered on again or in a
 * warm reset reads as empty. `hub_port_settled` is 1 once the hub runs and
 * the port has been connected for the debounce time. `hub_port_reset_begin`
 * asks for a reset and returns 0, or -1 when the hub is not running;
 * `hub_port_reset_done` returns 1 when the reset finished, 0 while it runs
 * and -1 when it failed.
 */

int hub_port_status(struct xhci_ctrl *hc, int owner, int port, struct usb_port_status *out);
int hub_port_settled(struct xhci_ctrl *hc, int owner, int port);
int hub_port_reset_begin(struct xhci_ctrl *hc, int owner, int port);
int hub_port_reset_done(struct xhci_ctrl *hc, int owner, int port);

#endif /* _USB_HUB_H_ */
