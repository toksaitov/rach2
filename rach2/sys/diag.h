#ifndef _DIAG_H_
#define _DIAG_H_

/*
 * diag: the boot report the `rach2.diag` command line asks for.
 *
 * A machine with no serial port answers only on its screen, so the numbers a
 * first bring-up needs must be printed where a camera or a capture card can
 * read them: where the kernel ends, where the bootloader put each module, how
 * much memory survived the reservations, and which Ethernet silicon is in the
 * slot. Everything here reads and prints; nothing here configures a device.
 */

/**
 * Print the boot layout and the Ethernet identity to the console and serial.
 *
 * Called after every boot-time reservation, the page cache's included, so the
 * frame count is the memory the machine actually has left, and before any
 * driver has configured a device. Every device read is bounded and read-only:
 * the Ethernet peek maps the controller's memory window and reads one
 * register, and it changes no PCI command bit, so a boot that runs this and a
 * boot that does not hand the same hardware state to the drivers that follow.
 */
void boot_diag_report(void);

#endif /* _DIAG_H_ */
