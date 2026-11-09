/*
 * sw_sync.c: software timelines whose fences a test signals, debugfs's
 * sync/sw_sync. A stub until 76-3a writes it, which makes none.
 */

#include "../drm/drm.h"

#include "../lib/kerrno.h"

int32_t sw_sync_open(fd_entry_t *e)
{
    (void)e;
    return -ENODEV;
}
