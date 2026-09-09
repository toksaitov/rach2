/*
 * The calls the buffer cache and the FAT drivers make into the block layer
 * on the host: the most a device takes in one request, read from the record
 * itself, since a host test's devices have no partition views over them,
 * and the device's flush, which a fake device has nothing to do for.
 */

#include "block/block.h"

uint32_t block_max_sectors(block_device_t *device)
{
    return device->max_sectors ? device->max_sectors : BLOCK_REQ_SECTORS;
}

/** The device's flush on the host: a fake device holds nothing back. */
int block_flush(block_device_t *device)
{
    (void)device;
    return 0;
}
