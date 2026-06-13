#include "vm.h"

#include "../lib/kdef.h"

/* A VMA boundary must be page-aligned (4 KB). */
#define VM_PAGE_MASK 0xFFFu

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
}

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
