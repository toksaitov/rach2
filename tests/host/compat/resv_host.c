/* The reservation's reader of its write fence for a host test that links
 * the display core without dmabuf/dmabuf.c (dmabuf.h): one thread, so the
 * fence is read without the lock. */
#include "../../../rach2/sys/dmabuf/dmabuf.h"

/** The write fence with a reference, or NULL. */
struct dma_fence *dma_resv_get_write(struct dma_resv *resv)
{
    return dma_fence_get(resv->write);
}
