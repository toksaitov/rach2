#include "heap.h"
#include "paging.h"
#include "../serial.h"

uintptr_t mem_reserved_ptr;

void init_mm(void)
{
    uintptr_t kernel_end = (uintptr_t) &k_phys_mem_end;

    /*
     * Set mem_reserved_ptr to the kernel end, unless it's already
     * been set to a higher value (e.g., to skip past modules).
     */
    if (mem_reserved_ptr < kernel_end) {
        mem_reserved_ptr = kernel_end;
    }
}

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

    /* Check if we're exceeding identity-mapped memory */
    if (new_ptr > HARD_MEMORY_LIMIT) {
        serial_printf("mem_reserve: OUT OF MEMORY! ptr=0x%X size=0x%X limit=0x%X\n",
                      mem_reserved_ptr, size, HARD_MEMORY_LIMIT);
        return 0;
    }

    /*
     * If the frame bitmap is initialized, check if any frames in this
     * range are already used (e.g., by alloc_user_page). If so, skip
     * past them and try again. This prevents mem_reserve from allocating
     * over memory already assigned to user pages.
     */
    if (_frames != NULL) {
        uintptr_t start_frame = result / PAGE_SIZE;
        uintptr_t end_frame = (new_ptr + PAGE_SIZE - 1) / PAGE_SIZE;
        uintptr_t f;

        for (f = start_frame; f < end_frame && f < _frames_number; f++) {
            if (_frames[f]) {
                /* Frame already in use - skip past it and retry */
                mem_reserved_ptr = (f + 1) * PAGE_SIZE;
                goto retry;
            }
        }

        /* Mark all frames as used */
        for (f = start_frame; f < end_frame && f < _frames_number; f++) {
            _frames[f] = 1;
        }
    }

    mem_reserved_ptr = new_ptr;

    return result;
}
