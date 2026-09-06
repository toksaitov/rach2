/*
 * Memory pressure on the host: the one number the buffer cache asks of it,
 * enough buffers for a test that fills the cache twice over, and the
 * interrupt state, which a host test never enters.
 */

#include "mm/pressure.h"

uint32_t mem_cache_target(void)
{
    return 128;
}

int in_interrupt(void)
{
    return 0;
}

uint32_t mem_reserve_frames(void)
{
    return 0;
}

uint32_t frame_free_count(void)
{
    return 1u << 20;
}
