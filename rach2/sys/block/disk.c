/*
 * disk.c: the disk registry, its records, and the scan of a disk's
 * partition table into views.
 */

#include "disk.h"
#include "diskinfo.h"
#include "../klog.h"
#include "../fs/bcache.h"
#include "../fs/devfs.h"
#include "../lib/kerrno.h"
#include "../proc/wait_queue.h"

static struct disk disks[DISK_MAX];
static int ndisks;

/** Copy IDENTIFY's string at `first_word` into `out`, or make it empty when
 * there is no IDENTIFY answer. */
static void ata_string_or_empty(char *out, int chars, const volatile uint16_t *id, int first_word)
{
    if (id) {
        diskinfo_ata_string(out, chars, id, first_word);
    } else {
        out[0] = '\0';
    }
}

/**
 * Every byte of a disk is counted in 64 bits, from its raw window's size to
 * the offsets of its reads, so a device whose size would wrap is not
 * registered. A device registered again, a driver brought up again from the
 * debug shell, keeps its number and its nodes. Otherwise a released record
 * is taken before a fresh one, so a stick plugged, pulled and plugged takes
 * the lowest number free; its nodes were made the first time and are named
 * again for the table. A disk with no model string is logged as giving none.
 */
struct disk *disk_register(block_device_t *dev, const char *transport, int location,
                           const volatile uint16_t *identify)
{
    struct disk *d = 0;
    char count[24];
    int i, again = 0;

    if (!dev) {
        return 0;
    }
    if (dev->sector_count > ~(uint64_t)0 / BLOCK_SECTOR_SIZE) {
        klog(KLOG_WARN, "disk", "%s %d: not registered: more sectors than 64 bits of bytes count",
             transport, location);
        return 0;
    }
    for (i = 0; i < ndisks; i++) {
        if (disks[i].dev == dev) {
            d = &disks[i];
            again = 1;
        }
    }
    for (i = 0; !d && i < ndisks; i++) {
        if (disks[i].released) {
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
    d->released = 0;
    d->removable = 0;
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
    (void)diskinfo_u64_dec(count, (int)sizeof(count), dev->sector_count);
    if (d->model[0] != '\0') {
        klog(KLOG_INFO, "disk", "disk%d: %s %d: %s sectors, model %s", d->number, transport,
             location, count, d->model);
    } else {
        klog(KLOG_INFO, "disk", "disk%d: %s %d: %s sectors, and it gives no model", d->number,
             transport, location, count);
    }
    return d;
}

struct disk *disk_at(int n)
{
    if (n < 0 || n >= ndisks || disks[n].released) {
        return 0;
    }
    return &disks[n];
}

void disk_set_model(struct disk *d, const char *vendor, const char *product)
{
    int n = 0, i;

    for (i = 0; vendor[i] && n < DISK_MODEL_CHARS; i++) {
        d->model[n++] = vendor[i];
    }
    if (n > 0 && n < DISK_MODEL_CHARS && product[0]) {
        d->model[n++] = ' ';
    }
    for (i = 0; product[i] && n < DISK_MODEL_CHARS; i++) {
        d->model[n++] = product[i];
    }
    d->model[n] = '\0';
}

void disk_release(struct disk *d)
{
    int i;

    d->present = 0;
    d->released = 1;
    d->dev = 0;
    d->backing = 0;
    d->table.kind = PART_UNREAD;
    d->table.count = 0;
    d->whole.present = 0;
    for (i = 0; i < DISK_PARTS; i++) {
        d->views[i].present = 0;
    }
    d->opens = 0;
    d->writer_held = 0;
}

const char *disk_table_name(const struct disk *d)
{
    switch (d->table.kind) {
    case PART_MBR:
        return "mbr";
    case PART_GPT:
        return "gpt";
    case PART_GPT_BAD:
        return "gpt-bad";
    case PART_NONE:
        return d->table.volume ? "one volume" : "none";
    default:
        return "unread";
    }
}

struct disk *disk_of(block_device_t *dev)
{
    block_device_t *physical = dev ? block_physical(dev) : 0;
    int i;

    for (i = 0; i < ndisks; i++) {
        if (!disks[i].released && disks[i].dev == physical) {
            return &disks[i];
        }
    }
    return 0;
}

int disk_count(void)
{
    return ndisks;
}

/** The `part_read_fn` of a scan: read through the device `ctx`. */
static int read_backing(void *ctx, uint64_t lba, uint32_t count, void *buf)
{
    return block_request((block_device_t *)ctx, 0, lba, count, buf);
}

/* One work area, held by the scan that has it: a scan reads through the
 * device and sleeps there, so two `BLKRRPART` calls on two disks would
 * otherwise write over each other's table between the header it checked
 * and the entries it checks against it. A caller that finds it held
 * waits. */
static uint8_t scan_work[PART_WORK_BYTES];
static int scan_busy;
static wait_queue_t scan_wq;

/** The wait condition of a scan: no other scan holds the work area. */
static int scan_free(void *arg)
{
    (void)arg;
    return !scan_busy;
}

/**
 * The buffer cache keys a block by its device and its number within that
 * device, so a view that moves to another place on the medium must leave no
 * block of its old place behind. A view with a held or dirty block is not
 * moved, since `disk_rescan` refuses while anything is open, and the rest
 * are dropped here.
 */
void disk_set_table(struct disk *d, const struct part_table *table, block_device_t *backing)
{
    int i;

    for (i = 0; i < DISK_PARTS; i++) {
        if (d->views[i].present) {
            (void)bcache_invalidate(&d->views[i].part.device);
        }
        d->views[i].present = 0;
    }
    d->backing = backing;
    if (table != &d->table) {
        d->table = *table;
    }
    for (i = 0; i < d->table.count; i++) {
        const struct part_entry *e = &d->table.parts[i];

        block_partition_init(&d->views[i].part, block_lag_partition_backing(backing, e->start_lba),
                             e->start_lba, e->sector_count);
        d->views[i].present = 1;
    }
    devfs_disk_part_nodes(d);
}

/**
 * The scan sleeps, so the views go out of use before it, and their cached
 * blocks go with them: the table about to be read may put a view somewhere
 * else on the medium, and `disk_set_table` no longer sees these views as
 * present to drop them itself. The table is read through the device itself,
 * which answers at boot where no process could sleep on the lag device; the
 * views go over `backing`.
 */
int disk_scan(struct disk *d, block_device_t *backing)
{
    int i, rc;

    for (i = 0; i < DISK_PARTS; i++) {
        if (d->views[i].present) {
            (void)bcache_invalidate(&d->views[i].part.device);
        }
        d->views[i].present = 0;
    }
    d->backing = backing;
    while (scan_busy) {
        wq_wait_event_nointr(&scan_wq, scan_free, 0);
    }
    scan_busy = 1;
    rc = part_scan(&d->table, read_backing, d->dev, d->dev->sector_count, scan_work);
    scan_busy = 0;
    wq_wake_all(&scan_wq);
    if (rc != 0) {
        klog(KLOG_WARN, "disk", "disk%d: the first sector could not be read", d->number);
        return -EIO;
    }
    disk_set_table(d, &d->table, backing);
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
    return 0;
}

int disk_rescan(struct disk *d)
{
    int rc;

    if (d->opens > 1 || devfs_disk_claimed(d)) {
        return -EBUSY;
    }
    rc = disk_scan(d, d->backing);
    if (rc != 0) {
        return rc;
    }
    klog(KLOG_INFO, "disk", "disk%d: table read again: %d partitions", d->number, d->table.count);
    return 0;
}
