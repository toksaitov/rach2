#include "../klog.h"
#include "heap.h"
#include "paging.h"
#include "../serial.h"

uintptr_t mem_reserved_ptr;

/**
 * Anchor the bump allocator at the end of the kernel image.
 *
 * `mem_reserved_ptr` is the next free physical address. The boot code may
 * already have raised it past loaded modules, so only advance it, never
 * lower it.
 */
void init_mm(void)
{
    uintptr_t kernel_end = (uintptr_t)&_end;

    if (mem_reserved_ptr < kernel_end) {
        mem_reserved_ptr = kernel_end;
    }
}

/**
 * Carve `size` bytes off the top of reserved physical memory.
 *
 * A monotonic bump allocator over identity-mapped memory: it returns the
 * current break and advances it, never freeing. With `align` set the break
 * is first rounded up to a page boundary.
 *
 * Once the frame bitmap exists this allocator and the page allocator
 * (`alloc_user_page`) share the same physical range, so the reservation must
 * avoid frames already handed out. Any used frame in the span moves the break
 * past it and the search restarts; on success every covered frame is claimed.
 *
 * @return The reserved physical address, or 0 if the request would cross
 *         `HARD_MEMORY_LIMIT`, the top of identity-mapped memory.
 */
uintptr_t mem_reserve(size_t size, bool align)
{
    uintptr_t result;
    uintptr_t new_ptr;

retry:
    if (align && (mem_reserved_ptr & 0xFFF)) {
        mem_reserved_ptr &= 0xFFFFF000;
        mem_reserved_ptr += 0x1000;
    }

    result = mem_reserved_ptr;
    new_ptr = mem_reserved_ptr + size;

    if (new_ptr > HARD_MEMORY_LIMIT) {
        klog(KLOG_ERR, "mem", "reserve: out of memory: at 0x%X, %d bytes, limit 0x%X",
             mem_reserved_ptr, size, HARD_MEMORY_LIMIT);
        return 0;
    }

    if (_frames != NULL) {
        uintptr_t start_frame = result / PAGE_SIZE;
        uintptr_t end_frame = (new_ptr + PAGE_SIZE - 1) / PAGE_SIZE;
        uintptr_t f;

        for (f = start_frame; f < end_frame && f < _frames_number; f++) {
            if (_frames[f]) {
                mem_reserved_ptr = (f + 1) * PAGE_SIZE;
                goto retry;
            }
        }

        for (f = start_frame; f < end_frame && f < _frames_number; f++) {
            paging_mark_reserved(f);
        }
    }

    mem_reserved_ptr = new_ptr;

    return result;
}
