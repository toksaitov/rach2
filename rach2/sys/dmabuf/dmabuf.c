/*
 * dmabuf.c: dma-bufs, buffers shared between devices, processes and the
 * CPU through descriptors. A stub until 76-3a writes it, which makes none.
 */

#include "../drm/drm.h"

int is_dma_buf_file(const fd_entry_t *e)
{
    (void)e;
    return 0;
}
