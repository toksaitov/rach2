/*
 * The one call the buffer cache makes into the block layer's device tree on
 * the host: the most a device takes in one request, read from the record
 * itself, since a host test's devices have no partition views over them.
 */

#include "block/block.h"

uint32_t block_max_sectors(block_device_t *device)
{
    return device->max_sectors ? device->max_sectors : BLOCK_REQ_SECTORS;
}
