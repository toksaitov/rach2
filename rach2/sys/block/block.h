#ifndef _BLOCK_H_
#define _BLOCK_H_

#include "../lib/kint.h"

/** The sector size every block device in Rach2 uses. */
#define BLOCK_SECTOR_SIZE 512

typedef struct block_device block_device_t;

/**
 * A sector-addressed storage device behind a small read/write vtable.
 *
 * `read` and `write` transfer `count` whole sectors starting at logical block
 * address `lba`, to or from `buffer`, and return 0 on success or a negative
 * errno (`-EIO`) on failure. They are synchronous and polled: a caller may
 * invoke them with interrupts off and without the scheduler running. `drvdata`
 * is the backend's private pointer.
 */
struct block_device {
    uint32_t sector_size;
    uint64_t sector_count;
    int (*read)(block_device_t *device, uint64_t lba, uint32_t count,
                void *buffer);
    int (*write)(block_device_t *device, uint64_t lba, uint32_t count,
                 const void *buffer);
    void *drvdata;
};

/**
 * The whole-disk boot device, or `NULL` until `block_probe` finds a disk.
 */
extern block_device_t *block0;

/**
 * Bring up the boot block device and publish it as `block0`.
 *
 * Selects the AHCI driver when `rach2.disk=ahci` is on the kernel command line
 * (falling back to ATA if no AHCI HBA is present), and the ATA driver otherwise.
 * Returns 0 when a device answered and `block0` is set, or -1 when none did.
 * Polled and IRQ-free, so it is safe to call before the scheduler runs.
 */
int block_probe(void);

/**
 * A partition: the sector range `[start_lba, start_lba + sector_count)` of a
 * backing device, presented as its own `block_device`.
 *
 * The view's `read`/`write` add `start_lba` to each request and forward it to
 * the backing device, rejecting any request that runs past the partition. The
 * `device` member is first, so a `block_device_t *` handed to the vtable can be
 * recovered as the enclosing `block_partition_t`.
 */
typedef struct block_partition {
    block_device_t device;
    block_device_t *backing;
    uint64_t start_lba;
} block_partition_t;

/**
 * Initializes `partition` as the `sector_count`-sector slice of `backing` that
 * starts at `start_lba`. After this, `&partition->device` is a usable device.
 */
void block_partition_init(
         block_partition_t *partition,
         block_device_t *backing,
         uint64_t start_lba,
         uint64_t sector_count
     );

#endif
