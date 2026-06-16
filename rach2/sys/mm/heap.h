#ifndef _HEAP_H_
#define _HEAP_H_

#include "../lib/kint.h"
#include "../lib/kstd.h"

/** End of the kernel image in the physical address space; set by the linker script. */
extern uintptr_t k_phys_mem_end;

/** Next free physical address; the bump allocator's break. */
extern uintptr_t mem_reserved_ptr;

/** Anchor the bump allocator at the end of the kernel image. */
void init_mm(void);

/** Reserve `size` bytes of physical memory, page-aligned if `align`; 0 on failure. */
uintptr_t mem_reserve(size_t size, bool align);

#endif
