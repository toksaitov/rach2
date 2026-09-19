#ifndef _VM_H_
#define _VM_H_

#include "../lib/kint.h"
#include "../lib/kdef.h"

/*
 * The address-space model: a process's memory is described by a small set of
 * virtual memory areas (VMAs), each a half-open page-aligned range with a
 * protection and a kind. This is the data structure real kernels (Linux
 * vm_area_struct, BSD vm_map_entry) are built on; here a growable array
 * rather than a tree, since a teaching process has few regions. The
 * demand-paging fault handler consults it to decide whether a fault is a page
 * to lazily allocate, a stack to grow, or a segfault. See the Readme.
 *
 * vm.c is pure logic (no kernel globals), so the host test exercises the
 * find/insert/remove/overlap operations directly.
 */

/* Policy: the regions a process may hold at most. A process's regions are an
 * array in the heap that starts at VMA_FIRST entries and doubles as it
 * fills; at the limit a further mapping answers ENOMEM, the bound Linux
 * keeps as max_map_count. An entry is 16 bytes, so a process at the limit
 * holds a megabyte of them. A program starts with a region per loaded
 * segment and one for its stack, and the heap's comes with its first brk;
 * the rest are anonymous mmaps, which is what a C library's allocator
 * reaches for, one per large allocation. `tests/musl/vmamusl.c`
 * takes three hundred of them, past the sixteen the array starts with. */
#define VMA_LIMIT 65536
#define VMA_FIRST 16

/* How far below its base the main stack VMA may grow on demand (8 MB). */
#define VM_STACK_MAX_GROWTH 0x800000u

/* Anonymous mmap regions are placed by bumping down from here (well below the
 * stack-growth window, well above the heap), mirroring a Linux mmap_base. */
#define VM_MMAP_BASE 0xB0000000u

/* Region protection bits (mirror the PROT_* / page-table permission flags). */
#define VM_PROT_READ  0x1u
#define VM_PROT_WRITE 0x2u
#define VM_PROT_EXEC  0x4u

/* Region kind; drives both teaching clarity and fault policy. */
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
    vma_t *vmas; /* `cap` entries in the heap, `count` of them in use */
    uint32_t cap;
    uint32_t count;
    uint32_t heap_start;  /* initial program break (base of the heap VMA) */
    uint32_t brk;         /* current program break (top of the heap VMA)  */
    uint32_t stack_limit; /* lowest address the stack VMA may grow down to */
    uint32_t mmap_top;    /* next anonymous mmap is placed just below here */
} vm_space_t;

/* Reset an address space to empty; the array it has, if any, is kept. */
void vm_init(vm_space_t *vm);

/* Give the array back; the space is empty after. */
void vm_free(vm_space_t *vm);

/* Make `dst`, which has no array, a copy of `src`; 0, or -1 with no memory. */
int vm_copy(vm_space_t *dst, const vm_space_t *src);

/* Return the VMA containing 'addr' (start <= addr < end), or NULL. */
vma_t *vm_find(vm_space_t *vm, uint32_t addr);

/* Return 1 if any existing VMA intersects [start, end), else 0. */
int vm_overlaps(const vm_space_t *vm, uint32_t start, uint32_t end);

/** Insert a region [start, end) with 'prot'/'type'; 0 ok, -1 on error,
 * which includes the limit reached and a heap with no room to grow. */
int vm_insert(vm_space_t *vm, uint32_t start, uint32_t end, uint32_t prot, vm_type_t type);

/** Remove [start, end) (munmap semantics); 0 ok, -1 if a split has no room. */
int vm_remove(vm_space_t *vm, uint32_t start, uint32_t end);

/** Bytes the regions cover together: what RLIMIT_AS is measured against. */
uint32_t vm_total(const vm_space_t *vm);

/** Grow the stack VMA down to cover 'page' (page-aligned; the fault handler
 * floors the fault address); 1 if grown, 0 if not. */
int vm_grow_stack(vm_space_t *vm, uint32_t page, uint32_t as_limit);

#endif /* _VM_H_ */
