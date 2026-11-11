/**
 * Host test of the TLB shootdowns that mm/paging.c asks for, with i686's
 * `cpu.h`; tlb64_hosttest.c runs the same rows with x86-64's.
 *
 * host-deps: rach2/sys/mm/vm.c rach2/sys/mm/frame.c rach2/sys/mm/notify.c
 * host-deps: rach2/sys/mm/pin.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/spinlock_host.c
 * host-cflags: -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
 * host-cflags: -U__UINTPTR_TYPE__ -D__UINTPTR_TYPE__=unsigned
 * host-cflags: -U__INTPTR_TYPE__ -D__INTPTR_TYPE__=int
 * host-cflags: -U__SIZE_TYPE__ -D__SIZE_TYPE__=unsigned
 * host-cflags: -U__PTRDIFF_TYPE__ -D__PTRDIFF_TYPE__=int
 *
 * mm_model.h includes paging.c whole over a model of the page tables and
 * counts the shootdowns, as Linux 6.12 would give them. The rows: a
 * demand-faulted page and a page mapped where none was cost none, and a
 * page mapped over a present one costs one; an unmap costs one for the
 * call, of the span unmapped up to 33 pages and of every user translation
 * past that, and frees no frame before it, and an unmap of nothing costs
 * none; an mprotect of many pages costs one, and so does a fork, for the
 * parent's pages it makes copy-on-write.
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#include "mm_model.h"

#define P  ((uintptr_t)PAGE_SIZE)
#define RW (VM_PROT_READ | VM_PROT_WRITE)

#ifdef RACH2_HOST_X86_64
#define NAME "tlb64-hosttest"
#else
#define NAME "tlb-hosttest"
#endif

/* The frames an unmap row mapped, which must keep their reference until its
 * shootdown, and how many. */
static uintptr_t held[80];
static unsigned int held_count;

/** At an unmap's shootdown: no page of the span is present, and every frame
 * the row mapped still has its reference. */
static void unmap_check(const struct page_dir *dir, uintptr_t start, uintptr_t end)
{
    unsigned int i;

    (void)start;
    (void)end;
    for (i = 0; i < held_count; i++) {
        check(!(pte_of((page_dir_t *)dir, WIN_BASE + i * P) & PTE_P),
              "no page is present at the shootdown");
        check(frame_maps(held[i] / PAGE_SIZE) == 1, "no frame is freed before the shootdown");
    }
}

/** A page faulted in or mapped where none was costs no shootdown; one
 * mapped over a present page costs one. */
static void map_rows(void)
{
    uintptr_t second = alloc_user_frame();
    uint32_t before;

    check(vm_insert(&mm_a.vm, WIN_BASE, WIN_BASE + 4 * P, RW, VM_ANON) == 0, "a region");
    before = shootdowns;
    check(vm_resolve_in(&mm_a, WIN_BASE, 1, ~(uintptr_t)0) == 1 &&
              vm_resolve_in(&mm_a, WIN_BASE + P, 0, ~(uintptr_t)0) == 1,
          "two pages fault in");
    check(alloc_user_page(&dir_a, WIN_BASE + 2 * P) != 0, "a page is allocated");
    check(shootdowns == before, "a page where none was costs no shootdown");
    check(map_user_frame(&dir_a, WIN_BASE + 2 * P, second, PTE_P | PTE_U) == 0 &&
              shootdowns == before + 1,
          "a page mapped over a present one costs one");
    vm_remove(&mm_a.vm, WIN_BASE, WIN_BASE + 4 * P);
    unmap_user_range(&dir_a, WIN_BASE, WIN_BASE + 4 * P);
    check(frame_maps(second / PAGE_SIZE) == 0, "the unmap frees the page mapped over");
}

/** An unmap of `count` pages costs one shootdown, of the span up to the
 * ceiling and of everything past it, frees each frame after it, and a
 * second unmap of the range costs none. */
static void unmap_rows(unsigned int count)
{
    uintptr_t end = WIN_BASE + count * P;
    unsigned int i;
    uint32_t before;

    for (i = 0; i < count; i++) {
        held[i] = alloc_user_page(&dir_a, WIN_BASE + i * P);
        check(held[i] != 0, "a page to unmap");
    }
    held_count = count;
    shot_check = unmap_check;
    before = shootdowns;
    unmap_user_range(&dir_a, WIN_BASE - 2 * P, end + 2 * P);
    shot_check = NULL;
    held_count = 0;
    check(shootdowns == before + 1, "an unmap costs one shootdown");
    if (count <= PAGING_FLUSH_CEILING) {
        check(shot_start == WIN_BASE && shot_end == end, "of the span unmapped");
    } else {
        check(shot_start == CPU_TLB_ALL, "of every user translation past the ceiling");
    }
    for (i = 0; i < count; i++) {
        check(frame_maps(held[i] / PAGE_SIZE) == 0 && pte_of(&dir_a, WIN_BASE + i * P) == 0,
              "each frame is freed and its entry cleared after the shootdown");
    }
    unmap_user_range(&dir_a, WIN_BASE, end);
    check(shootdowns == before + 1, "an unmap of nothing costs none");
}

/** An mprotect of 8 pages costs one shootdown, and a fork one, for the
 * parent's pages it makes copy-on-write. */
static void protect_fork_rows(void)
{
    uintptr_t end = WIN_BASE + 8 * P;
    uintptr_t va;
    uint32_t before;

    check(vm_insert(&mm_a.vm, WIN_BASE, end, RW, VM_ANON) == 0, "a region of 8 pages");
    for (va = WIN_BASE; va < end; va += P) {
        check(vm_resolve_in(&mm_a, va, 1, ~(uintptr_t)0) == 1, "a page faults in");
    }
    before = shootdowns;
    paging_protect_range(&dir_a, WIN_BASE, end, VM_PROT_READ);
    check(shootdowns == before + 1 && shot_start == WIN_BASE && shot_end == end &&
              !(pte_of(&dir_a, WIN_BASE + 3 * P) & PTE_W),
          "an mprotect of 8 pages costs one shootdown");
    paging_protect_range(&dir_a, WIN_BASE, end, RW);
    before = shootdowns;
    check(paging_fork_copy(&mm_a, &dir_b) == 0 && vm_copy(&mm_b.vm, &mm_a.vm) == 0, "fork copies");
    check(shootdowns == before + 1 && shot_start == WIN_BASE && shot_end == end &&
              (pte_of(&dir_a, WIN_BASE + 5 * P) & PTE_COW),
          "a fork costs one shootdown for the parent's copy-on-write pages");
    vm_remove(&mm_a.vm, WIN_BASE, end);
    unmap_user_range(&dir_a, WIN_BASE, end);
    vm_remove(&mm_b.vm, WIN_BASE, end);
    unmap_user_range(&dir_b, WIN_BASE, end);
}

/** The protection keys are off in the host test, as on a processor without
 * them. */
int fpu_pkeys(void)
{
    return 0;
}

/** PKRU with the keys off. */
uint32_t fpu_pkru(void)
{
    return 0;
}

/** No region here maps a disk file, whose mapping a pin would hold. */
int filemap_disk_pages(const struct vm_file *f)
{
    (void)f;
    return 0;
}

/** Run every row; 0 when each passes, 1 otherwise. */
int main(void)
{
    model_start(NAME);
    map_rows();
    unmap_rows(1);
    unmap_rows(PAGING_FLUSH_CEILING);
    unmap_rows(PAGING_FLUSH_CEILING + 1);
    unmap_rows(80);
    protect_fork_rows();
    return model_end();
}
