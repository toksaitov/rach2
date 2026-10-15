/*
 * The boot allocator: physical memory taken from the end of the kernel image
 * up, below HARD_MEMORY_LIMIT, and never given back. The boot's page tables
 * come from it, and on i686 the frame table. The one caller after the boot
 * is the RTL8139 driver, for its receive ring at the card's first bring-up:
 * the ring is physically contiguous, which no other allocator gives. A
 * caller whose memory is to go back takes it from the heap instead.
 */
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

/** The boot allocator's break, the next free physical address. The boot
 * raises it past the modules before `init_mm`. */
extern uintptr_t mem_reserved_ptr;

/** Start the boot allocator at the end of the kernel image, or leave its
 * break where it is if the boot already raised it past that. */
void init_mm(void);

/**
 * Take `size` bytes of physical memory at the break and move the break past
 * them; with `align` the break is first rounded up to a page. Nothing taken
 * is given back. Once the frame table exists, the bytes avoid every frame
 * in use and each frame they touch is marked used.
 *
 * @return The physical address taken, or 0, with a record, when the bytes
 *         would pass HARD_MEMORY_LIMIT, the top of the boot's window.
 */
uintptr_t mem_reserve(size_t size, bool align);

/** The same reservation as the kernel's address of it (`phys_to_virt`),
 * for a caller that fills a structure there; NULL on failure. */
void *mem_reserve_ptr(size_t size, bool align);

#endif
