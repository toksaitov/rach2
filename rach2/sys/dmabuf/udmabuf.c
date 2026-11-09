/*
 * udmabuf.c: /dev/udmabuf, which makes dma-bufs of a process's sealed
 * memfds. A stub until 76-3a writes it, which registers no node.
 */

#include "../drm/drm.h"

int32_t udmabuf_start(void)
{
    return 0;
}
