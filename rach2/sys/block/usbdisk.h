#ifndef _USBDISK_H_
#define _USBDISK_H_

#include "../lib/kint.h"

/*
 * usbdisk: a stick that has finished bring-up, made into a disk.
 *
 * The class driver reports a stick from the pump, where nothing may sleep,
 * so the partition table is read the way the pump does everything: one
 * request started, and asked after on the passes that follow, until the
 * table is in hand. Publishing the nodes and reclaiming a stick that has
 * gone touch what a process in a syscall may be changing, so those two run
 * at a syscall entry or on a pump pass that did not interrupt a syscall,
 * whichever comes first; the counts they read are exact there, because a
 * syscall is not preempted and a sleeping process has recorded what it
 * holds.
 *
 * A stick is `/dev/usbN`, with `N` the lowest free number when it arrives,
 * and its partitions are `/dev/usbNp1` to `p4`. The number and the class
 * driver's slot are not the same thing: a stick that has gone keeps its
 * number until the last hold on it is released, so that nothing plugged in
 * afterwards is given a name something else still has open.
 */

/** How many sticks are named at once; one number per stick the class drives. */
#define USBDISK_MAX 8

/** The stick in class-driver slot `slot` is up. Called from the pump. */
void usbdisk_arrived(int slot, int lun);

/** The stick in slot `slot` has gone. Called from the pump. */
void usbdisk_left(int slot);

/**
 * One pass from the pump: advance the table being read, and publish or
 * reclaim when no process is inside the kernel.
 */
void usbdisk_step(void);

/**
 * Publish and reclaim what is waiting for a process context.
 *
 * The syscall entry calls it before every dispatch. It returns at once when
 * there is nothing to do and never sleeps: the reading is the pump's, and
 * what is left is bookkeeping.
 */
void usbdisk_service(void);

/**
 * How many holds are left on the stick numbered `n`: its mounts and what is
 * open on them, the descriptions on its nodes, its requests in flight or
 * quarantined, on the whole device and on each view, and the cache buffers
 * held on it. Zero is a stick that can be reclaimed.
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
