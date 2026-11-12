/*
 * Page frames on the host, for ext2's reads into the file page cache's
 * frames: a frame's address is its host address, so a map of it is the
 * address itself, and a read into frames is one request a frame through the
 * test's own `block_request`.
 */

#include "block/block.h"
#include "lib/kstring.h"
#include "mm/paging.h"

/** The frame's host address: a host frame is mapped where it is. */
void *kmap_temp(uintptr_t phys)
{
    return (void *)phys;
}

/** Nothing to unmap: `kmap_temp` maps nothing. */
void kunmap_temp(void *va)
{
    (void)va;
}

/** Clear the page at `phys`. 0. */
int paging_zero_frame(uintptr_t phys)
{
    k_memset((void *)phys, 0, PAGE_SIZE);
    return 0;
}

/**
 * Read `count` sectors from `lba` into `frames`, eight to a frame, as the
 * kernel's `block_read_frames` does, one request a frame. 0 or the first
 * request's error.
 */
int block_read_frames(block_device_t *device, uint64_t lba, uint32_t count, const uintptr_t *frames)
{
    uint32_t i, n;
    int rc;

    for (i = 0; i * 8u < count; i++) {
        n = count - i * 8u < 8u ? count - i * 8u : 8u;
        rc = block_request(device, 0, lba + i * 8u, n, (void *)frames[i]);
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}
