/**
 * Dual-target seam test for the VMA address-space operations (mm/vm.c).
 *
 * host-deps: rach2/sys/mm/vm.c
 *
 * Exercises insert with overlap/alignment/slot rejection, find by address,
 * overlap detection, and the four munmap cases (full cover, left trim, right
 * trim, middle split). Returns the failed-check count (0 == all passed). Run
 * natively under AddressSanitizer/UBSan.
 */

#include "mm/vm.h"

static int fails;

#define CHECK(cond)    \
    do {               \
        if (!(cond)) { \
            fails++;   \
        }              \
    } while (0)

#define PG 0x1000u

int main(void)
{
    vm_space_t vm;
    vma_t *r;

    vm_init(&vm);
    CHECK(vm.count == 0);
    CHECK(vm_find(&vm, 0x10000) == NULL);

    /* Basic insert + find. */
    CHECK(vm_insert(&vm, 0x10000, 0x14000, VM_PROT_READ | VM_PROT_WRITE, VM_DATA) == 0);
    CHECK(vm.count == 1);
    r = vm_find(&vm, 0x10000);
    CHECK(r != NULL && r->start == 0x10000 && r->end == 0x14000);
    CHECK(vm_find(&vm, 0x13fff) == r);
    CHECK(vm_find(&vm, 0x14000) == NULL); /* end is exclusive */
    CHECK(vm_find(&vm, 0x0ffff) == NULL);

    /* Reject empty/inverted, unaligned, and overlapping inserts. */
    CHECK(vm_insert(&vm, 0x20000, 0x20000, 0, VM_ANON) == -1); /* empty */
    CHECK(vm_insert(&vm, 0x21000, 0x20000, 0, VM_ANON) == -1); /* inverted */
    CHECK(vm_insert(&vm, 0x20100, 0x21000, 0, VM_ANON) == -1); /* unaligned start */
    CHECK(vm_insert(&vm, 0x20000, 0x21100, 0, VM_ANON) == -1); /* unaligned end */
    CHECK(vm_insert(&vm, 0x13000, 0x15000, 0, VM_ANON) == -1); /* overlaps */
    CHECK(vm.count == 1);

    /* Overlap predicate. */
    CHECK(vm_overlaps(&vm, 0x0f000, 0x11000) == 1);
    CHECK(vm_overlaps(&vm, 0x14000, 0x16000) == 0); /* abuts, no overlap */
    CHECK(vm_overlaps(&vm, 0x12000, 0x13000) == 1);

    /* A second region, then munmap full cover. */
    CHECK(vm_insert(&vm, 0x20000, 0x24000, VM_PROT_READ, VM_ANON) == 0);
    CHECK(vm.count == 2);
    CHECK(vm_remove(&vm, 0x20000, 0x24000) == 0); /* full cover -> drop */
    CHECK(vm.count == 1);
    CHECK(vm_find(&vm, 0x21000) == NULL);
    CHECK(vm_find(&vm, 0x11000) != NULL); /* the other survived */

    /* Left trim: remove the low part of [0x10000,0x14000). */
    CHECK(vm_remove(&vm, 0x0f000, 0x11000) == 0);
    r = vm_find(&vm, 0x11000);
    CHECK(r != NULL && r->start == 0x11000 && r->end == 0x14000);
    CHECK(vm_find(&vm, 0x10000) == NULL);

    /* Right trim: remove the high part. */
    CHECK(vm_remove(&vm, 0x13000, 0x20000) == 0);
    r = vm_find(&vm, 0x11000);
    CHECK(r != NULL && r->start == 0x11000 && r->end == 0x13000);
    CHECK(vm_find(&vm, 0x13000) == NULL);

    /* Middle split: insert a 4-page region and punch the middle two pages out,
     * leaving the low and high pages as two separate regions. */
    CHECK(vm.count == 1); /* [0x11000,0x13000) */
    CHECK(vm_insert(&vm, 0x40000, 0x44000, VM_PROT_READ | VM_PROT_WRITE, VM_ANON) == 0);
    CHECK(vm.count == 2);
    CHECK(vm_remove(&vm, 0x41000, 0x43000) == 0);
    CHECK(vm.count == 3);
    {
        vma_t *lo = vm_find(&vm, 0x40000);
        vma_t *hi = vm_find(&vm, 0x43000);
        CHECK(lo != NULL && lo->start == 0x40000 && lo->end == 0x41000);
        CHECK(hi != NULL && hi->start == 0x43000 && hi->end == 0x44000);
        CHECK(vm_find(&vm, 0x42000) == NULL); /* the punched-out middle */
    }

    /* Removing a non-overlapping range is a no-op. */
    CHECK(vm_remove(&vm, 0x80000, 0x81000) == 0);
    CHECK(vm.count == 3);

    /* Stack growth: a one-page stack at the top, growable down to the
     * stack_limit. */
    {
        vm_space_t s;
        vma_t *st;

        vm_init(&s);
        s.stack_limit = 0xBF7FF000u;
        CHECK(vm_insert(&s, 0xBFFFF000u, 0xC0000000u, VM_PROT_READ | VM_PROT_WRITE, VM_STACK) == 0);

        /* A page just below the stack grows it down by one page. */
        CHECK(vm_grow_stack(&s, 0xBFFFE000u) == 1);
        st = vm_find(&s, 0xBFFFE000u);
        CHECK(st != NULL && st->start == 0xBFFFE000u && st->end == 0xC0000000u);
        CHECK(s.count == 1); /* grown in place, not a new region */

        /* A page far below (but still at/above the limit) jumps start down to
         * it; the gap pages are now inside the region and fault in lazily. */
        CHECK(vm_grow_stack(&s, 0xBF800000u) == 1);
        CHECK(vm_find(&s, 0xBF800000u) != NULL);
        CHECK(vm_find(&s, 0xBFF00000u) != NULL); /* a gap page, now covered */

        /* At the limit is allowed; below it is refused. */
        CHECK(vm_grow_stack(&s, 0xBF7FF000u) == 1);
        CHECK(vm_grow_stack(&s, 0xBF7FE000u) == 0);
        CHECK(vm_find(&s, 0xBF7FF000u) != NULL);
        CHECK(vm_find(&s, 0xBF7FE000u) == NULL);

        /* A page at or above the current start is not a growth (already mapped
         * region or above the stack entirely). */
        CHECK(vm_grow_stack(&s, 0xBF7FF000u) == 0); /* equals start now */

        /* With no stack region, growth always fails. */
        vm_init(&s);
        CHECK(vm_grow_stack(&s, 0xBFFFE000u) == 0);

        /* Growth must not swallow another region sitting in the growth window:
         * a (hostile) segment mapped just below the stack blocks growth past
         * it. */
        vm_init(&s);
        s.stack_limit = 0xBF7FF000u;
        CHECK(vm_insert(&s, 0xBFFFF000u, 0xC0000000u, VM_PROT_READ | VM_PROT_WRITE, VM_STACK) == 0);
        CHECK(vm_insert(&s, 0xBF900000u, 0xBF901000u, VM_PROT_READ, VM_DATA) == 0);
        /* One page down is still clear: allowed. */
        CHECK(vm_grow_stack(&s, 0xBFFFE000u) == 1);
        /* Down to/over the planted region is refused; the stack stays put. */
        CHECK(vm_grow_stack(&s, 0xBF900000u) == 0);
        CHECK(vm_grow_stack(&s, 0xBF8FF000u) == 0);
        CHECK(vm_find(&s, 0xBFFFE000u) != NULL && vm_find(&s, 0xBF900000u) != NULL);
        CHECK(vm_find(&s, 0xBF8FF000u) == NULL); /* stack did not jump the region */
    }

    return (fails > 100) ? 100 : fails;
}
