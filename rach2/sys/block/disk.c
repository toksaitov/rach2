/*
 * The disk registry. `disk.h` says what it is for.
 */

#include "disk.h"
#include "diskinfo.h"
#include "../klog.h"
#include "../fs/devfs.h"
#include "../lib/kerrno.h"

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
    d->table.kind = PART_UNREAD;
    d->table.count = 0;
    d->backing = dev;
    for (i = 0; i < DISK_PARTS; i++) {
        d->views[i].present = 0;
    }
    d->whole.present = 0;
    d->opens = 0;
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

static int read_backing(void *ctx, uint64_t lba, uint32_t count, void *buf)
{
    return block_request((block_device_t *)ctx, 0, lba, count, buf);
}

/* One work area: the scans run one at a time, at boot and from a system
   call that cannot be preempted. */
static uint8_t scan_work[PART_WORK_BYTES];

void disk_scan(struct disk *d, block_device_t *backing)
{
    int i;

    for (i = 0; i < DISK_PARTS; i++) {
        d->views[i].present = 0;
    }
    d->backing = backing;
    /* The table is read through the device itself, which answers at boot
     * where no process could sleep on the lag device; the views go over
     * `backing`. */
    if (part_scan(&d->table, read_backing, d->dev, d->dev->sector_count, scan_work) != 0) {
        klog(KLOG_WARN, "disk", "disk%d: the first sector could not be read", d->number);
        return;
    }
    for (i = 0; i < d->table.count; i++) {
        const struct part_entry *e = &d->table.parts[i];

        block_partition_init(&d->views[i].part, block_lag_partition_backing(backing, e->start_lba),
                             e->start_lba, e->sector_count);
        d->views[i].present = 1;
    }
    devfs_disk_part_nodes(d);
    if (d->table.refused != 0) {
        klog(KLOG_WARN, "disk", "disk%d: %d partition entries refused", d->number,
             d->table.refused);
    }
    if (d->table.backup_used) {
        klog(KLOG_WARN, "disk", "disk%d: the primary GPT is damaged; the backup served", d->number);
    }
    if (d->table.kind == PART_GPT_BAD) {
        klog(KLOG_WARN, "disk", "disk%d: a GPT whose headers both fail; no partitions", d->number);
    }
}

int disk_rescan(struct disk *d)
{
    if (d->opens > 1 || devfs_disk_claimed(d)) {
        return -EBUSY;
    }
    disk_scan(d, d->backing);
    klog(KLOG_INFO, "disk", "disk%d: table read again: %d partitions", d->number, d->table.count);
    return 0;
}
