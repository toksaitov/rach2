#ifndef _USB_HUB_H_
#define _USB_HUB_H_

#include "../lib/kint.h"

/*
 * hub: the hub class driver.
 *
 * A hub is a device whose ports the core enumerates like the root
 * controller's. The core hands a configured hub here; this driver reads its
 * descriptor, powers its ports, watches its change endpoint, and answers the
 * core's port operations for the devices below it.
 */

/** How many hubs this kernel drives at once. */
#define HUB_MAX 2

struct usb_port_status;
struct usb_completion;

/**
 * Take a hub the core has configured. `index` is the hub's device index in
 * the core, `device` the controller's handle, `interface` the interface
 * number, `protocol` the interface's protocol byte, and `ep_dci` the change
 * endpoint's handle. Returns 0 when the hub was taken, -1 when refused.
 */
int hub_attach(int index, int device, int interface, int protocol, int ep_dci, int depth);

/** Give up a hub that has gone or that the core is closing. */
void hub_detach(int index);

/** Step every hub by `elapsed_ms`, from the pump. */
void hub_step(uint32_t elapsed_ms);

/** Take a completion that names a hub's endpoint; 1 when it did. */
int hub_completion(const struct usb_completion *completion);

/* The port operations a device below a hub answers to, with `owner` the
 * hub's device index and `port` numbered from zero. */
int hub_port_status(int owner, int port, struct usb_port_status *out);
int hub_port_settled(int owner, int port);
int hub_port_reset_begin(int owner, int port);
int hub_port_reset_done(int owner, int port);

#endif /* _USB_HUB_H_ */
