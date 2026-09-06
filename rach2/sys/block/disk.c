/*
 * The disk registry. `disk.h` says what it is for.
 */

#include "disk.h"
#include "diskinfo.h"
#include "../klog.h"
#include "../fs/devfs.h"

static struct disk disks[DISK_MAX];
static int ndisks;

static void ata_string_or_empty(char *out, int chars, const volatile uint16_t *id, int first_word)
{
    if (id) {
        diskinfo_ata_string(out, chars, id, first_word);
    } else {
        out[0] = '\0';
    }
}

struct disk *disk_register(block_device_t *dev, const char *transport, int location,
                           const volatile uint16_t *identify)
{
    struct disk *d = 0;
    int i, again = 0;

    if (!dev) {
        return 0;
    }
    /* A driver brought up again from the debug shell registers the same
     * device records; the disk keeps its number and its nodes. */
    for (i = 0; i < ndisks; i++) {
        if (disks[i].dev == dev) {
            d = &disks[i];
            again = 1;
        }
    }
    if (!d && ndisks >= DISK_MAX) {
        klog(KLOG_WARN, "disk", "%s %d: not registered: the registry has %d disks", transport,
             location, DISK_MAX);
        return 0;
    }
    if (!d) {
        d = &disks[ndisks];
        d->number = ndisks;
        ndisks++;
    }
    d->present = 1;
    d->dev = dev;
    d->transport = transport;
    d->location = location;
    ata_string_or_empty(d->model, DISK_MODEL_CHARS, identify, 27);
    ata_string_or_empty(d->serial, DISK_SERIAL_CHARS, identify, 10);
    ata_string_or_empty(d->firmware, DISK_FIRMWARE_CHARS, identify, 23);
    d->table = DISK_TABLE_UNREAD;
    d->mbr_id = 0;
    for (i = 0; i < DISK_PARTS; i++) {
        d->parts[i].type = 0;
        d->parts[i].start_lba = 0;
        d->parts[i].sector_count = 0;
        d->views[i].present = 0;
    }
    d->whole.present = 0;
    d->writer_held = 0;
    if (d->number == 0 && identify) {
        diskinfo_record_identify(transport, location, identify, dev->sector_count);
    }
    if (!again) {
        devfs_disk_nodes(d);
    }
    klog(KLOG_INFO, "disk", "disk%d: %s %d: %d sectors, model %s", d->number, transport, location,
         (uint32_t)dev->sector_count, d->model);
    return d;
}

struct disk *disk_at(int n)
{
    if (n < 0 || n >= ndisks) {
        return 0;
    }
    return &disks[n];
}

struct disk *disk_of(block_device_t *dev)
{
    block_device_t *physical = dev ? block_physical(dev) : 0;
    int i;

    for (i = 0; i < ndisks; i++) {
        if (disks[i].dev == physical) {
            return &disks[i];
        }
    }
    return 0;
}

int disk_count(void)
{
    return ndisks;
}

static uint32_t mbr_disk_id(const uint8_t *sector)
{
    return (uint32_t)sector[440] | ((uint32_t)sector[441] << 8) | ((uint32_t)sector[442] << 16) |
           ((uint32_t)sector[443] << 24);
}

void disk_set_table(struct disk *d, const struct mbr_part *parts, block_device_t *backing)
{
    int i;

    for (i = 0; i < DISK_PARTS; i++) {
        d->parts[i] = parts[i];
        if (parts[i].sector_count == 0) {
            d->views[i].present = 0;
            continue;
        }
        block_partition_init(&d->views[i].part,
                             block_lag_partition_backing(backing, parts[i].start_lba),
                             parts[i].start_lba, parts[i].sector_count);
        d->views[i].present = 1;
    }
}

void disk_scan(struct disk *d)
{
    static uint8_t sector[BLOCK_SECTOR_SIZE];
    struct mbr_part parts[DISK_PARTS];
    int i;

    for (i = 0; i < DISK_PARTS; i++) {
        d->views[i].present = 0;
    }
    if (d->dev->read(d->dev, 0, 1, sector) != 0) {
        d->table = DISK_TABLE_UNREAD;
        klog(KLOG_WARN, "disk", "disk%d: the first sector could not be read", d->number);
        return;
    }
    d->mbr_id = mbr_disk_id(sector);
    if (mbr_parse(sector, parts) != 0) {
        d->table = DISK_TABLE_NONE;
        return;
    }
    d->table = DISK_TABLE_MBR;
    disk_set_table(d, parts, d->dev);
}
