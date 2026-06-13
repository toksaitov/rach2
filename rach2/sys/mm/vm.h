#ifndef _VM_H_
#define _VM_H_

#include "../lib/kint.h"
#include "../lib/kdef.h"

/*
 * The address-space model: a process's memory is described by a small set of
 * virtual memory areas (VMAs), each a half-open page-aligned range with a
 * protection and a kind. This is the data structure real kernels (Linux
 * vm_area_struct, BSD vm_map_entry) are built on -- here a fixed array rather
 * than a tree, since a teaching process has only a handful of regions. The
 * demand-paging fault handler consults it to decide whether a fault is a page
 * to lazily allocate, a stack to grow, or a segfault. See the Readme.
 *
 * vm.c is pure logic (no kernel globals), so the host seam test exercises the
 * find/insert/remove/overlap operations directly.
 */

/* Maximum regions per process. */
#define VMA_MAX 16

/* How far below its base the main stack VMA may grow on demand (8 MB). */
#define VM_STACK_MAX_GROWTH 0x800000u

/* Anonymous mmap regions are placed by bumping down from here (well below the
 * stack-growth window, well above the heap), mirroring a Linux mmap_base. */
#define VM_MMAP_BASE 0xB0000000u

/* Region protection bits (mirror the PROT_* / page-table permission flags). */
#define VM_PROT_READ  0x1u
#define VM_PROT_WRITE 0x2u
#define VM_PROT_EXEC  0x4u

/* Region kind -- drives both teaching clarity and fault policy. */
typedef enum {
    VM_NONE = 0,
    VM_CODE,  /* ELF text: read + execute */
    VM_DATA,  /* ELF data/bss: read + write */
    VM_HEAP,  /* brk-managed heap */
    VM_STACK, /* the main stack (grows down) */
    VM_ANON   /* anonymous mmap */
} vm_type_t;

/* A single virtual memory area: [start, end) page-aligned. */
typedef struct vma {
    uint32_t start; /* inclusive, page-aligned */
    uint32_t end;   /* exclusive, page-aligned */
    uint32_t prot;  /* VM_PROT_* */
    vm_type_t type;
} vma_t;

/* The per-process address space. */
typedef struct vm_space {
    vma_t vmas[VMA_MAX];
    uint32_t count;
    uint32_t heap_start;  /* initial program break (base of the heap VMA) */
    uint32_t brk;         /* current program break (top of the heap VMA)  */
    uint32_t stack_limit; /* lowest address the stack VMA may grow down to */
    uint32_t mmap_top;    /* next anonymous mmap is placed just below here */
} vm_space_t;

/* Reset an address space to empty. */
void vm_init(vm_space_t *vm);

/* Return the VMA containing 'addr' (start <= addr < end), or NULL. */
vma_t *vm_find(vm_space_t *vm, uint32_t addr);

/* Return 1 if any existing VMA intersects [start, end), else 0. */
int vm_overlaps(const vm_space_t *vm, uint32_t start, uint32_t end);

/*
 * Insert a region [start, end) with 'prot'/'type'. Returns 0 on success, or -1
 * if the range is empty/unaligned, overlaps an existing region, or no slot is
 * free.
 */
int vm_insert(vm_space_t *vm, uint32_t start, uint32_t end, uint32_t prot, vm_type_t type);

/*
 * Remove the range [start, end) from the address space (munmap semantics): a
 * region fully covered is dropped, a partially overlapped one is trimmed, and a
 * range punched out of the middle splits its region in two. Returns 0 on
 * success, or -1 if the split would need more than VMA_MAX slots.
 */
int vm_remove(vm_space_t *vm, uint32_t start, uint32_t end);

/*
 * Stack-growth policy for the demand-paging fault handler. If the page-aligned
 * 'page' lies just below the stack VMA and at or above stack_limit, extend that
 * VMA's start down to 'page' and return 1; otherwise leave the map unchanged and
 * return 0. 'page' must already be page-aligned (the caller floors the fault
 * address). Pure: it only edits the VMA bookkeeping, never touches page tables.
 */
int vm_grow_stack(vm_space_t *vm, uint32_t page);

#endif /* _VM_H_ */
