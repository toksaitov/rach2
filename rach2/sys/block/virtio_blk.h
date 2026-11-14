/*
 * virtio_blk.h: the virtio block driver, as Linux's virtio_blk: each virtio
 * block device on the shared transport published as a disk of the block
 * layer, its requests carried on one virtqueue for each processor up to the
 * queues the device offers.
 *
 * The boot drives the devices by polling, as it drives AHCI and NVMe: the
 * reads that mount the root run before the scheduler, and a page fault with
 * interrupts off may read or write a page at any time after.
 * `virtio_blk_start` then moves the disks to requests completed by the
 * queues' interrupts and keeps the polled transfers for the fault path.
 * `sys/block/Readme.md` describes the queues, the two paths and the errors.
 */
#ifndef _VIRTIO_BLK_H_
#define _VIRTIO_BLK_H_

#include "../lib/kint.h"

/**
 * Find every virtio block device the transport took, up to eight, bring
 * each up and register it with the disk registry, as transport `virtio` at
 * its place among the devices found, publishing the first as `block0` when
 * no other driver has. Registers the driver for devices the transport
 * takes later, as a write to /sys/bus/pci/drivers/virtio-pci/bind does.
 *
 * A device brought up by an earlier call is not reset again: its disk may
 * be mounted. Its disk is registered again, which keeps its number.
 * Returns 0 when at least one disk is published, -1 otherwise. Polled, so
 * it may run before the scheduler; called from `block_probe`.
 */
int virtio_blk_init(void);

/**
 * Move every disk to requests completed by its queues' interrupts:
 * `submit`, `flush` and `BLOCK_FRAMES` in place of `BLOCK_FAULT_SAFE`, and
 * the discards and writes of zeroes the device offers. A disk keeps polling
 * under `rach2.vblkpoll` or when its transport gives it no interrupt, and
 * logs which. Called once at boot, after the root is mounted, with
 * interrupts on.
 */
void virtio_blk_start(void);

/**
 * Do the work that waited for a process: let go of each removed disk that
 * was held at its removal and that nothing holds any more, so that its
 * number is free again, and read the capacity after a configuration change
 * whose interrupt could not read it. Returns at once when there is none.
 * In process context under the big lock, at the entry of a system call, as
 * `usbdisk_service` is called.
 */
void virtio_blk_service(void);

/**
 * Stop every disk for the machine's stop, as Linux's virtio bus resets
 * each device at shutdown (virtio_dev_shutdown, drivers/virtio/virtio.c):
 * refuse new commands, finish the writes and the flushes the device has
 * and those that wait, flush a cache still marked unflushed, and reset the
 * device (`virtio_halt`), so that it reads no ring and writes into no
 * buffer while the machine restarts. Each disk logs
 * `virtio-blk: NAME: shutdown: reset with no request in flight`, or what
 * was left unanswered. Every later command answers EIO. Called from
 * `kernel_shutdown` once the file systems are unmounted, before the
 * processors stop.
 */
void virtio_blk_shutdown(void);

#endif
