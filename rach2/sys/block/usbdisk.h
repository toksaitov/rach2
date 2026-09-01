#ifndef _USBDISK_H_
#define _USBDISK_H_

#include "../lib/kint.h"

/*
 * usbdisk: a stick that has finished bring-up, made into a disk.
 *
 * The class driver reports a stick from the pump, where nothing may sleep,
 * and reading a partition table is a transfer that sleeps its caller. So
 * arrival and departure are recorded here and the work is done by the next
 * process to enter a syscall, through `usbdisk_service`. That is also where
 * a stick that has gone is reclaimed, because the count of what still holds
 * it can only fall while a process runs.
 *
 * A stick is `/dev/usbN`, with `N` the lowest free number when it arrives,
 * and its partitions are `/dev/usbNp1` to `p4`. The number and the class
 * driver's slot are not the same thing: a stick that has gone keeps its
 * number until the last hold on it is released, so that nothing plugged in
 * afterwards is given a name something else still has open.
 */

/** How many sticks are named at once; one number per stick the class drives. */
#define USBDISK_MAX 2

/** The stick in class-driver slot `slot` is up. Called from the pump. */
void usbdisk_arrived(int slot);

/** The stick in slot `slot` has gone. Called from the pump. */
void usbdisk_left(int slot);

/**
 * Do what an arrival or a departure asked for: read a table and publish the
 * nodes, or reclaim what a stick that has gone no longer holds.
 *
 * The syscall entry calls it before every dispatch. It returns at once when
 * there is nothing to do, and it may sleep when there is, which is why it is
 * called from there and not from the pump.
 */
void usbdisk_service(void);

/**
 * How many holds are left on the stick numbered `n`: its mounts and what is
 * open on them, the descriptions on its nodes, its requests in flight or
 * quarantined, and the cache buffers held on it. Zero is a stick that can be
 * reclaimed.
 */
int usbdisk_holds(int n);

/** How many sticks have been reclaimed since boot, for `/proc/usb`. */
uint32_t usbdisk_teardowns(void);

/**
 * One line of `/proc/usb` for the stick numbered `n`: the node name, what it
 * says it is, its size and its state. Returns the length, or 0 when there is
 * no stick with that number.
 */
int usbdisk_describe(int n, char *out, int size);

/** How many of the names are free right now. */
int usbdisk_free(void);

#endif /* _USBDISK_H_ */
