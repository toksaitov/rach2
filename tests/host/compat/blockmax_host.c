/*
 * The calls the buffer cache and the FAT drivers make into the block layer
 * on the host: the most a device takes in one request and its flags, read
 * from the record itself, since a host test's devices have no partition
 * views over them; the device's flush, which a fake device has nothing to do
 * for; and a request with flags, whose flags a test reads back.
 */

#include "block/block.h"

/** The device's own `max_sectors`, or BLOCK_REQ_SECTORS when it is 0. */
uint32_t block_max_sectors(block_device_t *device)
{
    return device->max_sectors ? device->max_sectors : BLOCK_REQ_SECTORS;
}

/* What a test reads back and sets: the flags of the last request made with
 * flags, the flushes asked for, and what each flush answers. */
uint32_t blockmax_last_flags;
int blockmax_flushes;
int blockmax_flush_rc;

/** The device's flush on the host: a fake device holds nothing back, and the
 * flush answers `blockmax_flush_rc`, which a test sets to make it fail. */
int block_flush(block_device_t *device)
{
    (void)device;
    blockmax_flushes++;
    return blockmax_flush_rc;
}

/** The device's flags: a host device has no partition view over it. */
uint32_t block_flags(block_device_t *device)
{
    return device->flags;
}

/** A request with flags on the host: a fake device keeps no cache of its
 * own, so a write without the flush is the write. */
int block_request_flags(block_device_t *device, int write, uint64_t lba, uint32_t count,
                        void *buffer, uint32_t flags)
{
    blockmax_last_flags = flags;
    return block_request(device, write, lba, count, buffer);
}
