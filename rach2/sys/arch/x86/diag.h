#ifndef _DIAG_H_
#define _DIAG_H_

/*
 * diag: the boot report the `rach2.diag` command line asks for.
 *
 * A machine with no serial port answers only on its screen, so the numbers a
 * first bring-up needs must be printed where a camera or a capture card can
 * read them: where the kernel ends, where the loader put each module, how
 * much memory is left after the reservations, and which Ethernet chip is on
 * the bus. Everything here reads and prints; nothing here configures a
 * device.
 */

/**
 * Log the boot layout and the Ethernet identity, which reach the screen and
 * the serial port.
 *
 * The boot calls it after every boot-time reservation, the page cache's
 * included, so that the frame count is the memory the machine has left, and
 * before any driver has configured a device; `boot_diag_hold` calls it again
 * at the end of the boot. Every device read is bounded and read-only: the
 * Ethernet peek maps the controller's memory window and reads one register,
 * and it changes no PCI command bit, so a boot that runs this and a boot
 * that does not hand the same hardware state to the drivers that follow.
 */
void boot_diag_report(void);

/**
 * Log the report again at the end of the boot, with a summary of what the
 * drivers found, and wait for a key on the 8042 or a byte on the serial
 * line. Input that arrived before the wait does not end it.
 *
 * For a machine whose only output is its screen: the boot log scrolls past in
 * under a second and cannot be photographed, so the numbers are shown once
 * more where nothing further will scroll over them.
 */
void boot_diag_hold(void);

#endif /* _DIAG_H_ */
