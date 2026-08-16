#include "block.h"
#include "ata.h"
#include "ahci.h"
#include "../serial.h"

#include "../cmdline.h"
#include "../lib/kerrno.h"

/* The whole-disk boot device, NULL until block_probe brings a driver up. */
block_device_t *block0 = 0;

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

int block_probe(void)
{
    /* rach2.nodisk skips the probe entirely, which is the switch a first boot
       on unfamiliar hardware needs: a controller that does not answer its
       reset holds this path for the whole of its bounded wait, and on a
       machine with no serial port the way to find out whether that is what
       stopped the boot is to leave the disk alone and see. */
    if (kcmdline_has("rach2.nodisk")) {
        serial_printf("block: skipped (rach2.nodisk)\n");
        return -1;
    }
    /* With rach2.disk=ahci, prefer the AHCI HBA and fall back to ATA if none
       answered; otherwise use ATA, so every existing IDE image boots exactly as
       before. kcmdline_has matches whole tokens, so the flag never collides
       with rach2.fsdisk. Both drivers publish block0 themselves. */
    if (kcmdline_has("rach2.disk=ahci") && ahci_init() == 0) {
        return 0;
    }
    return ata_init();
}
