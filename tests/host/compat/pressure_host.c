/*
 * Memory pressure on the host: the caches' target, which a test may raise,
 * no reserve, a free count a test may lower, and no interrupt context, which
 * a host test never enters.
 */

#include "mm/pressure.h"

/* The caches' target in frames. A test raises it to stand for a machine
 * whose memory share is more than the kernel heap holds, where the cache's
 * own cap is what stops it. */
uint32_t host_cache_target = 128;

/** The caches' target in frames: `host_cache_target`. */
size_t mem_cache_target(void)
{
    return host_cache_target;
}

/** Never in an interrupt handler: a host test has none. */
int in_interrupt(void)
{
    return 0;
}

/** No frames kept back: every allocation of a host test may take one. */
size_t mem_reserve_frames(void)
{
    return 0;
}

uint32_t host_free_frames = 1u << 20; /* a test lowers it to run the machine short */

/** The free frame count: `host_free_frames`. */
size_t frame_free_count(void)
{
    return host_free_frames;
}
