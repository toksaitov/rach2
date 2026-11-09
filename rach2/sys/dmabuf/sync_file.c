/*
 * sync_file.c: sync files, descriptors that carry fences between devices
 * and processes. A stub until 76-3a writes it, which knows of none.
 */

#include "../drm/drm.h"

struct dma_fence *sync_file_get_fence(int32_t fd)
{
    (void)fd;
    return NULL;
}
