/*
 * usbdisk.h: a USB stick's logical unit, once the class driver has brought
 * it up, made into a disk of the registry, and reclaimed once it has gone and
 * nothing holds it.
 *
 * The class driver reports a stick from the pump, where nothing may sleep,
 * so the partition table is read the way the pump does everything: one
 * request started, and checked on the passes that follow, until the
 * table is in hand. Registering a disk and reclaiming a stick that has gone
 * touch what a process in a system call may be changing, so those two run at
 * a system call's entry or on a pump pass that did not interrupt a system
 * call, whichever comes first; the counts they read are exact there, because
 * kernel code runs on one processor at a time and a system call is not
 * preempted, and a sleeping process has recorded what it holds.
 *
 * A stick is a disk of the registry, `/dev/diskN`, with `N` the lowest disk
 * number free when it arrives, and its partitions are `/dev/diskNpM`. A
 * stick that has gone keeps its number until the last hold on it is released,
 * so that nothing plugged in afterwards is given a name something else still
 * has open. Each logical unit made a disk has a record here, and the record's
 * number, which the calls below take, is neither the disk's number nor the
 * class driver's slot.
 */
#ifndef _USBDISK_H_
#define _USBDISK_H_

#include "../lib/kint.h"

/** How many records there are: logical units made disks at once. */
#define USBDISK_MAX 8

/**
 * Logical unit `lun` of the stick in the class driver's slot `slot` is up:
 * take a record for it, which the next settling registers. Called from the
 * pump; a unit that finds every record taken is logged and left.
 */
void usbdisk_arrived(int slot, int lun);

/**
 * The stick in the class driver's slot `slot` has gone: mark each record of
 * its units closing, or, when it has none, give the slot back to the class
 * driver at once. Called from the pump.
 */
void usbdisk_left(int slot);

/**
 * One pass of the pump: advance the table being read, and register and
 * reclaim when the pass did not interrupt a system call.
 */
void usbdisk_step(void);

/**
 * Register and reclaim what is waiting for a process context.
 *
 * The system call entry calls it before every dispatch, with interrupts on.
 * It returns at once when there is nothing to do and never sleeps: the
 * reading is the pump's, and what is left is bookkeeping.
 */
void usbdisk_service(void);

/**
 * How many holds are left on the stick of record `n`: the descriptions open
 * on its nodes, and on the whole device and on each view its mounts and
 * what is open on them, its requests in flight or quarantined, and the
 * cache buffers held on it. Zero is a stick that can be reclaimed, and the
 * answer for a free record or one with no disk yet.
 */
int usbdisk_holds(int n);

/** How many records have been reclaimed since boot, for `/proc/usb`. */
uint32_t usbdisk_teardowns(void);

/**
 * Write into `out`, of `size` bytes, one line of `/proc/usb` for the stick of
 * record `n`: the node name, what it says it is, its size in sectors, its
 * partitions, its state, its flushes, and its holds while it closes. Returns
 * the length, or 0 when there is no stick with that record, the class
 * driver's slot is free, or `size` is under 64.
 */
int usbdisk_describe(int n, char *out, int size);

/** How many records are free. */
int usbdisk_free(void);

#endif /* _USBDISK_H_ */
