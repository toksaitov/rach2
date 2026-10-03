/*
 * Memory pressure on the host: the one number the buffer cache asks of it,
 * enough buffers for a test that fills the cache twice over, and the
 * interrupt state, which a host test never enters.
 */

#include "mm/pressure.h"

/* The caches' target in frames. A test raises it to stand for a machine
 * whose memory share is more than the kernel heap holds, where the cache's
 * own cap is what stops it. */
uint32_t host_cache_target = 128;

size_t mem_cache_target(void)
{
    return host_cache_target;
}

int in_interrupt(void)
{
    return 0;
}

size_t mem_reserve_frames(void)
{
    return 0;
}

uint32_t host_free_frames = 1u << 20; /* a test lowers it to run the machine short */

size_t frame_free_count(void)
{
    return host_free_frames;
}
