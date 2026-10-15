/* heap.c: the boot allocator of heap.h. */

#include "../arch/arch.h"
#include "../klog.h"
#include "heap.h"
#include "paging.h"
#include "../serial.h"

uintptr_t mem_reserved_ptr;

void init_mm(void)
{
    uintptr_t kernel_end = (uintptr_t)&_end;

    if (mem_reserved_ptr < kernel_end) {
        mem_reserved_ptr = kernel_end;
    }
}

/**
 * Once the frame table exists, the frame allocator gives out frames of the
 * window too, so a used frame in the span moves the break past it and the
 * search starts again from there.
 */
uintptr_t mem_reserve(size_t size, bool align)
{
    uintptr_t result;
    uintptr_t new_ptr;

retry:
    if (align && (mem_reserved_ptr & (PAGE_SIZE - 1))) {
        mem_reserved_ptr &= ~(uintptr_t)(PAGE_SIZE - 1);
        mem_reserved_ptr += PAGE_SIZE;
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

void *mem_reserve_ptr(size_t size, bool align)
{
    uintptr_t at = mem_reserve(size, align);

    return at ? phys_to_virt(at) : NULL;
}
