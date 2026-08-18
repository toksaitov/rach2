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
 * Report a boot disk, probing only if none has been established.
 *
 * `block_probe` resets and re-identifies a controller every time it is called,
 * which is work a boot has no reason to repeat and which several callers do:
 * the disk mount, the diagnostic, and the device discovery that runs before
 * either. This answers from `block0` when a disk has already answered, so a
 * working controller is never reset a second time.
 *
 * A failure is deliberately not remembered. Caching one would mean a controller
 * that did not answer at boot could never be asked again, and the rescue
 * shell's whole purpose is to ask a machine questions after it has failed to
 * start normally.
 *
 * @return 0 when a device answered and `block0` is set, -1 when none did.
 */
int block_ready(void);

/**
 * How many commands each driver has issued to the device, and how many of those
 * were cache flushes.
 *
 * The raw windows count their own calls into the block interface, which says
 * how well devfs batched but nothing about what the driver did underneath: a
 * driver that broke one 64-sector request into 64 commands would look identical
 * from above and cost exactly the million cache flushes batching exists to
 * avoid. These count where the commands actually leave for the device.
 */
uint64_t block_commands(void);
uint64_t block_flushes(void);

/** Record one command, and separately one cache flush, as it is issued. */
void block_count_command(void);
void block_count_flush(void);

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
