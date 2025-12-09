#include "heap.h"

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

    if (align && (mem_reserved_ptr & 0xFFF)) {
        mem_reserved_ptr &= 0xFFFFF000;
        mem_reserved_ptr += 0x1000;
    }

    result = mem_reserved_ptr;
    mem_reserved_ptr += size;

    return result;
}
