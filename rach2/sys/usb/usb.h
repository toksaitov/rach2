#ifndef _USB_H_
#define _USB_H_

/**
 * Record what USB host controllers this machine has.
 *
 * Reads configuration space only: no register aperture is mapped and no
 * controller is touched, so this is safe to call before any driver exists and
 * on a machine whose firmware still owns the hardware. One record per
 * controller function, and for an Intel xHCI the port-routing registers as
 * well, because on that chipset the USB2 pins may be routed to the companion
 * EHCI and a controller that sees no port looks the same as a machine with
 * nothing plugged in.
 */
void usb_report_controllers(void);

#endif
