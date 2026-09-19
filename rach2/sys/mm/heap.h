#ifndef _HEAP_H_
#define _HEAP_H_

#include "../lib/kint.h"
#include "../lib/kstd.h"

/**
 * End of the kernel image in the physical address space. Both linkers that
 * build this kernel provide the symbol: `link.ld` assigns it for GNU ld, and
 * tcc defines it at the end of `.bss` on its own, which is why the kernel
 * reads this name and not a private one.
 */
extern uintptr_t _end;

/** Next free physical address; the bump allocator's break. */
extern uintptr_t mem_reserved_ptr;

/** Anchor the bump allocator at the end of the kernel image. */
void init_mm(void);

/** Reserve `size` bytes of physical memory, page-aligned if `align`; 0 on
 * failure. */
uintptr_t mem_reserve(size_t size, bool align);

/** The same reservation as a pointer, for a caller that fills a structure
 * there; NULL on failure. */
void *mem_reserve_ptr(size_t size, bool align);

#endif
