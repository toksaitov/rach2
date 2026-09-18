#ifndef _USB_H_
#define _USB_H_

#include "../lib/kint.h"

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

/**
 * How many times the timer's pump found work a message should have brought.
 *
 * Zero under the emulator, where a message sent has reached the processor by
 * the time the timer's pump asks. On hardware the event is in memory while
 * the message is still on its way, and a small count beside the vector's own
 * is that race rather than a lost edge. `/proc/interrupts` reports it, and
 * the suite that silences the controller on purpose requires it to rise.
 */
uint32_t usb_fallback_count(void);

/**
 * Note that a keystroke from this stack reached the line discipline.
 *
 * Called by the input layer. It is the third of the four stamps the latency
 * record prints; the fourth is taken where a reader takes the byte.
 */
void usb_stamp_wake(void);

/**
 * A reader took a byte: print what path the last keystroke took, once.
 *
 * Called by the terminal's read, which is where a reader's first sight of the
 * byte is. A byte that did not come from this stack prints nothing.
 */
void usb_stamp_read(void);

/** Whether `rach2.usblatency` is on, which is when the record is printed. */
int usb_stamp_recording(void);

/**
 * The name of a USB controller's register interface, from its PCI interface
 * byte: `uhci`, `ohci`, `ehci`, `xhci`, or `other` for a byte this kernel does
 * not know.
 */
const char *usb_interface_name(uint8_t prog_if);

#endif
