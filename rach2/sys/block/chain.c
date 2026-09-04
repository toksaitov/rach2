/*
 * How block devices stand on one another.
 *
 * A loop device's bytes live in a file on a volume, which lives on a disk, so
 * one device can be under another. The buffer cache reads this to decide
 * whether a caller holding one device's block may take another's, and the
 * attachment of a loop device reads it to refuse a chain longer than the four
 * buffers a process may hold can serve. Two functions and no state, in a file
 * of their own so that the cache's tests can have them without the drivers.
 */

#include "block.h"

int block_depth(block_device_t *device)
{
    int depth;

    for (depth = 0; device && device->backing; depth++) {
        if (depth > BLOCK_MAX_DEPTH) {
            return BLOCK_MAX_DEPTH + 1;
        }
        device = device->backing;
    }
    return depth;
}

int block_is_below(block_device_t *ancestor, block_device_t *device)
{
    int depth;

    if (!ancestor || !device) {
        return 0;
    }
    for (depth = 0; depth <= BLOCK_MAX_DEPTH; depth++) {
        device = device->backing;
        if (!device) {
            return 0;
        }
        if (device == ancestor) {
            return 1;
        }
    }
    return 0;
}
