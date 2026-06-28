#include "block.h"

#include "../lib/kerrno.h"

/**
 * Forwards a partition-relative read to the backing device, with the partition
 * offset added and the range checked against the partition's length.
 */
static int block_partition_read(
               block_device_t *device,
               uint64_t lba,
               uint32_t count,
               void *buffer
           )
{
    block_partition_t *partition = (block_partition_t *) device;

    if (count == 0 || lba > device->sector_count ||
        count > device->sector_count - lba) {
        return -EIO;
    }

    return partition->backing->read(
               partition->backing, partition->start_lba + lba, count, buffer
           );
}

/**
 * Forwards a partition-relative write to the backing device, with the partition
 * offset added and the range checked against the partition's length.
 */
static int block_partition_write(
               block_device_t *device,
               uint64_t lba,
               uint32_t count,
               const void *buffer
           )
{
    block_partition_t *partition = (block_partition_t *) device;

    if (count == 0 || lba > device->sector_count ||
        count > device->sector_count - lba) {
        return -EIO;
    }

    return partition->backing->write(
               partition->backing, partition->start_lba + lba, count, buffer
           );
}

void block_partition_init(
         block_partition_t *partition,
         block_device_t *backing,
         uint64_t start_lba,
         uint64_t sector_count
     )
{
    /* Clamp the view to what the backing device actually holds, so a forwarded
       `start_lba + lba` can never run past it or overflow. A start beyond the
       device yields an empty (zero-sector) view whose every request fails. */
    if (start_lba > backing->sector_count) {
        start_lba = backing->sector_count;
        sector_count = 0;
    } else if (sector_count > backing->sector_count - start_lba) {
        sector_count = backing->sector_count - start_lba;
    }

    partition->backing = backing;
    partition->start_lba = start_lba;

    partition->device.sector_size = backing->sector_size;
    partition->device.sector_count = sector_count;
    partition->device.read = block_partition_read;
    partition->device.write = block_partition_write;
    partition->device.drvdata = partition;
}
