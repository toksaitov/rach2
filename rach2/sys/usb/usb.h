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

/**
 * Bring up the USB stack.
 *
 * Finds a controller, takes it from the firmware, resets it, gives it its
 * rings and starts it, then lets the ports settle and records what is on them.
 * Returns 0 when a controller came up. A machine with no xHCI, or one whose
 * firmware will not give it up, is not an error: the record says what
 * happened and the kernel goes on without USB.
 *
 * Called once at boot, in thread context. It is the only part of the stack
 * that waits.
 */
int usb_init(void);

/**
 * Give the stack its turn.
 *
 * Called from the timer, so it takes what the controller has left and returns.
 * Nothing here waits on the controller.
 */
void usb_poll(void);

/** Stop the controller before the machine halts or restarts. */
void usb_stop(void);

#endif
