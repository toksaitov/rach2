/*
 * block.c: the disk probe, the partition views, and the drivers' command
 * and wait counters.
 */

#include "block.h"

#include "../klog.h"
#include "../platform.h"
#include "ahci.h"
#include "nvme.h"
#include "disk.h"
#include "../serial.h"

#include "../cmdline.h"
#include "../lib/kerrno.h"

block_device_t *block0 = 0;

/**
 * Pass a partition-relative read to the backing device, with the partition
 * offset added and the range checked against the partition's length.
 */
static int block_partition_read(block_device_t *device, uint64_t lba, uint32_t count, void *buffer)
{
    block_partition_t *partition = (block_partition_t *)device;

    if (count == 0 || lba > device->sector_count || count > device->sector_count - lba) {
        return -EIO;
    }

    return partition->backing->read(partition->backing, partition->start_lba + lba, count, buffer);
}

/**
 * Pass a partition-relative write to the backing device, with the partition
 * offset added and the range checked against the partition's length.
 */
static int block_partition_write(block_device_t *device, uint64_t lba, uint32_t count,
                                 const void *buffer)
{
    block_partition_t *partition = (block_partition_t *)device;

    if (count == 0 || lba > device->sector_count || count > device->sector_count - lba) {
        return -EIO;
    }

    return partition->backing->write(partition->backing, partition->start_lba + lba, count, buffer);
}

/** Pass a sleeping request to the backing device with the view's offset
 * added. The backing device completes the same request, so nothing needs
 * mapping back. */
static int block_partition_submit(block_device_t *device, struct block_request *request)
{
    block_partition_t *partition = (block_partition_t *)device;

    if (!partition->backing->submit || request->count == 0 || request->lba > device->sector_count ||
        request->count > device->sector_count - request->lba) {
        return -EIO;
    }
    request->lba += partition->start_lba;
    return partition->backing->submit(partition->backing, request);
}

uint32_t block_flags(block_device_t *device)
{
    while (device->read == block_partition_read) {
        device = ((block_partition_t *)device)->backing;
    }
    return device->flags;
}

uint32_t block_max_sectors(block_device_t *device)
{
    while (device->read == block_partition_read) {
        device = ((block_partition_t *)device)->backing;
    }
    if (device->max_sectors == 0) {
        return BLOCK_REQ_SECTORS;
    }
    return device->max_sectors > BLOCK_REQ_MAX_SECTORS ? BLOCK_REQ_MAX_SECTORS
                                                       : device->max_sectors;
}

/** Pass a flush to the backing device; installed only when it has one. */
static int block_partition_flush(block_device_t *device, struct block_request *request)
{
    block_partition_t *partition = (block_partition_t *)device;

    return partition->backing->flush(partition->backing, request);
}

void block_partition_init(block_partition_t *partition, block_device_t *backing, uint64_t start_lba,
                          uint64_t sector_count)
{
    if (start_lba > backing->sector_count) {
        start_lba = backing->sector_count;
        sector_count = 0;
    } else if (sector_count > backing->sector_count - start_lba) {
        sector_count = backing->sector_count - start_lba;
    }

    partition->backing = backing;
    partition->device.flags = backing->flags;
    partition->device.deadline_ticks = backing->deadline_ticks;
    partition->device.deadline_limit = backing->deadline_limit;
    partition->device.submit = block_partition_submit;
    partition->device.flush = backing->flush ? block_partition_flush : 0;
    partition->start_lba = start_lba;

    partition->device.sector_size = backing->sector_size;
    partition->device.sector_count = sector_count;
    partition->device.read = block_partition_read;
    partition->device.write = block_partition_write;
    partition->device.drvdata = partition;
}

int block_ready(void)
{
    if (block0) {
        return 0;
    }
    return block_probe();
}

/**
 * Disk 0's views go over the lag device when `rach2.blocklag` asks for one;
 * its whole-disk window keeps the real device.
 */
void block_scan_disks(void)
{
    int i;

    for (i = 0; i < disk_count(); i++) {
        struct disk *d = disk_at(i);

        if (d) {
            (void)disk_scan(d, i == 0 ? block_lag_wrap(d->dev) : d->dev);
        }
    }
}

/**
 * `rach2.nodisk` is for a first boot on unfamiliar hardware: a controller that
 * does not answer its reset holds the boot here for the whole of its bounded
 * wait, and on a machine with no serial port leaving the disks alone is the
 * way to tell whether that is what stopped the boot. `rach2.disk=ahci` is
 * accepted for the boot lines that have it and changes nothing.
 */
int block_probe(void)
{
    if (kcmdline_has("rach2.nodisk")) {
        klog(KLOG_INFO, "block", "skipped: rach2.nodisk");
        return -1;
    }
    klog(KLOG_INFO, "block", "started");
    block0 = 0;
    (void)ahci_init();
    (void)nvme_init();
    platform_disks_probe();
    if (!block0) {
        return -1;
    }
    block_scan_disks();
    return 0;
}

/*
 * The drivers' counters, for /proc/disk. A request of any size returns the
 * same answer however many commands carried it, and only the time differs;
 * these let a test require the batching without timing it.
 */
static uint64_t cmd_count;
static uint64_t flush_count;

void block_count_command(void)
{
    cmd_count++;
}

void block_count_flush(void)
{
    flush_count++;
}

uint64_t block_commands(void)
{
    return cmd_count;
}

uint64_t block_flushes(void)
{
    return flush_count;
}

static uint64_t poll_count;
static uint64_t wait_tick_count;

void block_count_poll(void)
{
    poll_count++;
}

void block_count_wait_tick(void)
{
    wait_tick_count++;
}

uint64_t block_polls(void)
{
    return poll_count;
}

uint64_t block_wait_ticks(void)
{
    return wait_tick_count;
}

block_device_t *block_submitter(block_device_t *dev)
{
    while (dev && dev->submit == block_partition_submit) {
        dev = ((block_partition_t *)dev->drvdata)->backing;
    }
    return dev;
}

block_device_t *block_physical(block_device_t *dev)
{
    for (;;) {
        block_device_t *under = block_lag_backing(dev);

        if (!under && dev && dev->submit == block_partition_submit) {
            under = ((block_partition_t *)dev->drvdata)->backing;
        }
        if (!under) {
            return dev;
        }
        dev = under;
    }
}
