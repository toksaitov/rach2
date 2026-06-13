/*
 * vm_hosttest.c
 *
 * host-deps: rach2/sys/mm/vm.c
 *
 * Dual-target seam test for the VMA address-space operations (mm/vm.c): insert
 * with overlap/alignment/slot rejection, find by address, overlap detection,
 * and the four munmap cases (full cover, left trim, right trim, middle split).
 * Returns the failed-check count (0 == all passed). Run natively under
 * AddressSanitizer/UBSan.
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

    return (fails > 100) ? 100 : fails;
}
