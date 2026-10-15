/*
 * usb.h: the USB stack's entry points for the rest of the kernel: the
 * controller inventory, bring-up, the timer's turn, the stop, and the
 * keystroke latency record.
 */

#ifndef _USB_H_
#define _USB_H_

#include "../lib/kint.h"

/**
 * Log the USB host controllers on the PCI bus.
 *
 * Reads configuration space only: no register aperture is mapped and no
 * controller is touched, so this is safe to call before any driver exists and
 * on a machine whose firmware still owns the hardware. One line per
 * controller function, and for an Intel xHCI the port-routing registers as
 * well, because on that chipset the USB 2 pins may be routed to the companion
 * EHCI, and a controller that sees no port looks the same as a machine with
 * nothing plugged in. Called at boot, before `usb_init`.
 */
void usb_report_controllers(void);

/**
 * Bring up the USB stack.
 *
 * Finds every xHCI and, for each, takes it from the firmware, resets it,
 * gives it its rings and starts it; one whose bring-up fails is given back and
 * left out. Then lets the ports settle, logs what is on them, waits up to
 * 400 ms for what is plugged in to be enumerated, and turns the controllers'
 * interrupts on. Returns 0 when a controller came up, and -1 otherwise. A
 * machine with no xHCI, or one whose firmware keeps it, is not an error: the
 * log says what happened and the kernel goes on without USB.
 *
 * Called once at boot, with interrupts on. It is the only part of the stack
 * that waits.
 */
int usb_init(void);

/**
 * Run the stack for one 10 ms tick: advance each controller's clock, drain
 * its events and step the devices.
 *
 * Called from the timer tick, with interrupts off; it takes what the
 * controllers have left and returns, and does nothing before `usb_init` has
 * brought a controller up or after `usb_stop`.
 */
void usb_poll(void);

/** Turn off every controller's interrupts and stop it, before the machine
 * halts or restarts; `usb_poll` does nothing afterwards. */
void usb_stop(void);

/**
 * The number of timer drains that took events with no USB interrupt pending.
 *
 * Counting starts when any USB interrupt source is enabled. A message
 * still in transit can increase this count. `/proc/interrupts` reports it.
 */
uint32_t usb_fallback_count(void);

/**
 * Note that a keystroke from this stack reached the input layer.
 *
 * Called by the HID driver after each code it hands on, inside the pump. It
 * is the third of the four stamps the latency record prints; the fourth is
 * taken where a reader takes the byte. Only the latest keystroke is kept, and
 * one from a pump that took no events clears it.
 */
void usb_stamp_wake(void);

/**
 * A reader took a byte: log the latest keystroke's times, once.
 *
 * Called by the terminal's read, which is where a reader first has the byte,
 * in process context; it turns interrupts off itself. A read while no
 * keystroke from this stack is pending, or while the record is off, logs
 * nothing.
 */
void usb_stamp_read(void);

/** Turn the latency record on or off and drop any reader or keystroke it
 * held, at USB bring-up. */
void usb_stamp_init(int enabled);

/** Record a pass of the pump, with interrupts off, before it delivers input:
 * `entry` is when the interrupt handler was last entered and `event` when
 * the drain ended, both in microseconds of `timer_now_us`; `msi` is whether
 * the controller's interrupt ran the pump, and `took` whether the drain took
 * events. */
void usb_stamp_pump(uint32_t entry, uint32_t event, int msi, int took);

/** Record `pid` as the console reader about to wait for input, with
 * interrupts off; the next keystroke is followed to that reader's dispatch. */
void usb_stamp_wait(uint32_t pid);

/** The scheduler has selected `pid`, with interrupts off: when it is the
 * reader a keystroke is followed to, the record's counts stop. */
void usb_stamp_dispatch(uint32_t pid);

/** Count `bytes` that `getrandom` produced while a keystroke's reader waited
 * to be selected, the work that ran ahead of it. Process context. */
void usb_stamp_work(uint32_t bytes);

/** Whether `rach2.usblatency` is on, which is when the record is printed. */
int usb_stamp_recording(void);

/**
 * The name of a USB controller's register interface, from its PCI interface
 * byte: `uhci`, `ohci`, `ehci`, `xhci`, or `other` for a byte this kernel does
 * not know.
 */
const char *usb_interface_name(uint8_t prog_if);

#endif
