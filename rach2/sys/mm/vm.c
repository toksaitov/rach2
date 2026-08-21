#include "vm.h"

#include "../lib/kdef.h"

/* A VMA boundary must be page-aligned (4 KB). */
#define VM_PAGE_MASK 0xFFFu

/**
 * Reset an address space to empty.
 *
 * Clears every VMA slot and zeroes the heap, brk, stack limit, and mmap
 * cursor. The space owns no mappings until vm_insert adds them.
 */
void vm_init(vm_space_t *vm)
{
    uint32_t i;

    for (i = 0; i < VMA_MAX; i++) {
        vm->vmas[i].start = 0;
        vm->vmas[i].end = 0;
        vm->vmas[i].prot = 0;
        vm->vmas[i].type = VM_NONE;
    }
    vm->count = 0;
    vm->heap_start = 0;
    vm->brk = 0;
    vm->stack_limit = 0;
    vm->mmap_top = 0;
}

/**
 * Return the VMA containing addr, or NULL if none does.
 *
 * Regions are half-open [start, end), so end is not part of the region.
 */
vma_t *vm_find(vm_space_t *vm, uint32_t addr)
{
    uint32_t i;

    for (i = 0; i < vm->count; i++) {
        if (addr >= vm->vmas[i].start && addr < vm->vmas[i].end) {
            return &vm->vmas[i];
        }
    }
    return NULL;
}

/**
 * Report whether [start, end) intersects any existing region.
 */
int vm_overlaps(const vm_space_t *vm, uint32_t start, uint32_t end)
{
    uint32_t i;

    for (i = 0; i < vm->count; i++) {
        /* Two half-open ranges intersect unless one is entirely below the
         * other. */
        if (start < vm->vmas[i].end && end > vm->vmas[i].start) {
            return 1;
        }
    }
    return 0;
}

/**
 * Add region [start, end) with the given protection and type.
 *
 * The range must be non-empty, page-aligned at both ends, and clear of every
 * existing region; there must be a free slot. Returns 0 on success or -1 if
 * any of those conditions fails.
 */
int vm_insert(vm_space_t *vm, uint32_t start, uint32_t end, uint32_t prot, vm_type_t type)
{
    vma_t *slot;

    if (start >= end) {
        return -1; /* empty or inverted range */
    }
    if ((start & VM_PAGE_MASK) != 0 || (end & VM_PAGE_MASK) != 0) {
        return -1; /* boundaries must be page-aligned */
    }
    if (vm->count >= VMA_MAX) {
        return -1; /* no free slot */
    }
    if (vm_overlaps(vm, start, end)) {
        return -1; /* would overlap an existing region */
    }

    slot = &vm->vmas[vm->count];
    slot->start = start;
    slot->end = end;
    slot->prot = prot;
    slot->type = type;
    vm->count++;
    return 0;
}

/**
 * Remove the span [start, end) from every region it touches.
 *
 * A fully covered region is dropped, a region clipped at one end is trimmed,
 * and a region straddling the span is split in two. VMA order is irrelevant,
 * so a dropped region's slot is filled from the end of the array. Returns 0,
 * or -1 if a split needs a free slot and none is available.
 */
int vm_remove(vm_space_t *vm, uint32_t start, uint32_t end)
{
    uint32_t i;

    if (start >= end) {
        return 0; /* nothing to remove */
    }

    i = 0;
    while (i < vm->count) {
        vma_t *r = &vm->vmas[i];

        /* Skip regions that do not overlap [start, end). */
        if (start >= r->end || end <= r->start) {
            i++;
            continue;
        }

        if (start <= r->start && end >= r->end) {
            /* Fully covered: drop it by swapping the last region into its slot
             * (VMA order is irrelevant) and re-check that slot. */
            *r = vm->vmas[vm->count - 1];
            vm->count--;
            continue;
        }
        if (start > r->start && end < r->end) {
            /* Punched out of the middle: split into [r->start, start) and
             * [end, r->end). */
            if (vm->count >= VMA_MAX) {
                return -1; /* no slot for the upper half */
            }
            vm->vmas[vm->count].start = end;
            vm->vmas[vm->count].end = r->end;
            vm->vmas[vm->count].prot = r->prot;
            vm->vmas[vm->count].type = r->type;
            vm->count++;
            r->end = start;
            i++;
            continue;
        }
        if (start <= r->start) {
            r->start = end; /* trim the low end */
        } else {
            r->end = start; /* trim the high end */
        }
        i++;
    }
    return 0;
}

/**
 * Extend a stack region downward to cover the faulting page.
 *
 * Returns 1 if a stack region was grown to include page, 0 otherwise. The page
 * must lie just below a VM_STACK region and at or above the stack limit, and
 * the growth window must not swallow any other region: a hostile ELF segment
 * or mmap inside that window must not be absorbed into the stack.
 */
int vm_grow_stack(vm_space_t *vm, uint32_t page)
{
    uint32_t i, j;

    for (i = 0; i < vm->count; i++) {
        vma_t *r = &vm->vmas[i];

        /* The faulting page must sit just below the stack region (which grows
         * down) and stay at or above the limit set when the stack was created.
         * Extending start covers every page from here up to the old start; the
         * intervening pages fault in lazily as the stack is actually touched.
         */
        if (r->type == VM_STACK && page < r->start && page >= vm->stack_limit) {
            /* The new span [page, r->start) must be clear of every other
             * region: a (possibly hostile) ELF segment or mmap inside the
             * growth window must not be swallowed by the stack. */
            for (j = 0; j < vm->count; j++) {
                if (j != i && page < vm->vmas[j].end &&
                    r->start > vm->vmas[j].start) {
                    return 0;
                }
            }
            r->start = page;
            return 1;
        }
    }
    return 0;
}
