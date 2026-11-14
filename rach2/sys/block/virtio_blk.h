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

#endif
