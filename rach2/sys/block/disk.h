#ifndef _DISK_H_
#define _DISK_H_

#include "../lib/kint.h"
#include "block.h"
#include "partition.h"

/*
 * The disk registry: every disk a driver has found, in the order found.
 *
 * A driver publishes a whole-disk `block_device` for each disk it brings
 * up and registers it here with its transport, its place on that transport,
 * and the IDENTIFY words it answered. The registry gives the disk its
 * number, which is the N of `/dev/diskN`, reads its partition table through
 * the partition service, and keeps the partition views the nodes and the
 * mounts use: view `i` is the table's entry `i`, and the node is named by
 * the entry's own number. Disk 0 is the first disk registered, which is
 * what `block0` points at, so the paths that boot from one disk are
 * unchanged.
 *
 * The raw writer's reservation is per disk: a reflash of one disk does not
 * lock out a kernel install on another, and the two boot windows of the
 * boot disk stay under that disk's reservation.
 */

#define DISK_MAX   8
#define DISK_PARTS PART_MAX

/* IDENTIFY string fields, in characters, as ATA lays them out. */
#define DISK_MODEL_CHARS    40 /* words 27-46 */
#define DISK_SERIAL_CHARS   20 /* words 10-19 */
#define DISK_FIRMWARE_CHARS 8  /* words 23-26 */

/** A raw window: a partition view and whether its node exists now. */
struct disk_window {
    int present;
    block_partition_t part;
};

struct disk {
    int present;
    int number;
    block_device_t *dev;    /* the whole disk, as the driver published it */
    const char *transport;  /* "ahci", "ata" */
    int location;           /* the AHCI port, or the ATA channel and drive */
    char model[DISK_MODEL_CHARS + 1];
    char serial[DISK_SERIAL_CHARS + 1];
    char firmware[DISK_FIRMWARE_CHARS + 1];
    struct part_table table;
    block_device_t *backing;               /* what the views are cut from */
    struct disk_window whole;              /* `/dev/diskN`, over `dev` */
    struct disk_window views[DISK_PARTS];  /* view `i` of `table.parts[i]` */
    int opens;                             /* descriptors open on its nodes */
    int removable; /* a stick: its whole window is always published, and it may go */
    int released;  /* gone and let go of; the number is free */
    /* The range the raw writer holds on this disk, while `writer_held`. */
    int writer_held;
    uint64_t writer_first, writer_last;
};

/**
 * Register a disk a driver has brought up.
 *
 * `identify` is the 256-word IDENTIFY answer, or NULL for a device that
 * gave none, whose strings are then empty. Returns the record, or NULL when
 * the registry is full; the driver keeps its device either way.
 */
struct disk *disk_register(block_device_t *dev, const char *transport, int location,
                           const volatile uint16_t *identify);

/**
 * Give `d` a model string that came from somewhere other than IDENTIFY: a
 * stick's INQUIRY vendor and product, joined by a space.
 */
void disk_set_model(struct disk *d, const char *vendor, const char *product);

/**
 * Let go of `d`: a disk that has gone and that nothing holds. Its number is
 * free for the next disk registered; its nodes stay made and are named
 * again for that disk.
 */
void disk_release(struct disk *d);

/** The table's kind as a word: `mbr`, `gpt`, `gpt-bad`, `none`, `unread`. */
const char *disk_table_name(const struct disk *d);

/** Disk `n`, or NULL when there is none; a released disk is NULL too. */
struct disk *disk_at(int n);

/** The disk whose medium `dev` is or stands on, or NULL. */
struct disk *disk_of(block_device_t *dev);

/** How many disks are registered. */
int disk_count(void);

/**
 * Read `d`'s partition table and publish its partition views over
 * `backing`, which is `d->dev` or the lag device wrapping it.
 *
 * Reads through `block_request`, which answers at once on a polled device,
 * so this runs where the drivers bring their disks up, before the
 * scheduler, and sleeps on a device that sleeps when a process calls it
 * later. A disk whose table cannot be read keeps no partitions.
 */
/**
 * Read `d`'s partition table again over `backing` and publish its views.
 *
 * @return 0, or `-EIO` when the first sector would not read, which leaves the
 *         disk with no partitions and its views withdrawn.
 */
int disk_scan(struct disk *d, block_device_t *backing);

/**
 * Give `d` a table read elsewhere, with its views over `backing`: a stick's,
 * read by the pump a sector at a time.
 */
void disk_set_table(struct disk *d, const struct part_table *table, block_device_t *backing);

/**
 * Read `d`'s table again, for a table rewritten under a running system.
 *
 * Refused with `-EBUSY` while a partition of the disk is claimed by a mount
 * or a node of the disk is open by anyone but the caller, because a view
 * that moved under a mount is corruption; the caller holds one descriptor,
 * the whole-disk node it asked through.
 *
 * @return 0 after the scan, or `-EIO` when the first sector would not read,
 *         which leaves the disk with no partitions and its views withdrawn: a
 *         caller told the table had been read would take that for a disk whose
 *         table is empty.
 */
int disk_rescan(struct disk *d);

#endif /* _DISK_H_ */
