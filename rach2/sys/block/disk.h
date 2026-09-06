#ifndef _DISK_H_
#define _DISK_H_

#include "../lib/kint.h"
#include "block.h"
#include "mbr.h"

/*
 * The disk registry: every disk a driver has found, in the order found.
 *
 * A driver publishes a whole-disk `block_device` for each disk it brings
 * up and registers it here with its transport, its place on that transport,
 * and the IDENTIFY words it answered. The registry gives the disk its
 * number, which is the N of `/dev/diskN`, reads its partition table, and
 * keeps the partition views the nodes and the mounts use. Disk 0 is the
 * first disk registered, which is what `block0` points at, so the paths
 * that boot from one disk are unchanged.
 *
 * The raw writer's reservation is per disk: a reflash of one disk does not
 * lock out a kernel install on another, and the two boot windows of the
 * boot disk stay under that disk's reservation.
 */

#define DISK_MAX   8
#define DISK_PARTS MBR_PART_COUNT

/* IDENTIFY string fields, in characters, as ATA lays them out. */
#define DISK_MODEL_CHARS    40 /* words 27-46 */
#define DISK_SERIAL_CHARS   20 /* words 10-19 */
#define DISK_FIRMWARE_CHARS 8  /* words 23-26 */

/* What the first sector turned out to be. */
#define DISK_TABLE_UNREAD 0 /* the read failed */
#define DISK_TABLE_NONE   1 /* read, but no 0x55AA signature */
#define DISK_TABLE_MBR    2

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
    int table; /* DISK_TABLE_* */
    uint32_t mbr_id;
    struct mbr_part parts[DISK_PARTS];
    struct disk_window whole;              /* `/dev/diskN`, over `dev` */
    struct disk_window views[DISK_PARTS];  /* `/dev/diskNpM` */
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

/** Disk `n`, or NULL when there is none. */
struct disk *disk_at(int n);

/** The disk whose medium `dev` is or stands on, or NULL. */
struct disk *disk_of(block_device_t *dev);

/** How many disks are registered. */
int disk_count(void);

/**
 * Read `d`'s first sector and publish its partition nodes.
 *
 * Polled, so it runs where the drivers bring their disks up, before the
 * scheduler. A disk whose sector cannot be read keeps no partitions.
 */
void disk_scan(struct disk *d);

/**
 * Give `d` the partition table `parts`, with the views over `backing`.
 *
 * The boot reads the boot disk's table once for the root and hands the same
 * table here, so the nodes and the mounted root never disagree, and its
 * views go over the lag device when `rach2.blocklag` asks for one.
 */
void disk_set_table(struct disk *d, const struct mbr_part *parts, block_device_t *backing);

#endif /* _DISK_H_ */
