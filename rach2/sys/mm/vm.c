/* vm.c: the regions of an address space, as vm.h describes them. */

#include "vm.h"

#include "../lib/kdef.h"
#include "../lib/kstring.h"
#include "kheap.h"

/* A VMA boundary must be page-aligned (4 KB). */
#define VM_PAGE_MASK  0xFFFu
#define VM_PAGE_SHIFT 12

/* The files whose last reference went, waiting for `vm_file_reap`. */
static struct vm_file *vm_dead;

void vm_file_get(struct vm_file *f)
{
    if (f) {
        f->refs++;
    }
}

void vm_file_put(struct vm_file *f)
{
    if (f && --f->refs == 0) {
        f->dead = vm_dead;
        vm_dead = f;
    }
}

void vm_file_reap(void)
{
    struct vm_file *f;

    while (vm_dead) {
        f = vm_dead;
        vm_dead = f->dead;
        f->dead = NULL;
        f->release(f);
    }
}

/** Drop the file reference of every region and clear its `file`. */
static void vm_drop_files(vm_space_t *vm)
{
    uint32_t i;

    for (i = 0; i < vm->count; i++) {
        vm_file_put(vm->vmas[i].file);
        vm->vmas[i].file = NULL;
    }
}

/** Cut region `r` at `at`, inside it, and return the part from `at` up for
 * the caller to place: it maps the file on from its own page and holds a
 * reference of its own. */
static vma_t vm_cut(vma_t *r, uintptr_t at)
{
    vma_t upper = *r;

    upper.start = at;
    if (upper.file) {
        upper.pgoff += (uint32_t)((at - r->start) >> VM_PAGE_SHIFT);
        vm_file_get(upper.file);
    }
    r->end = at;
    return upper;
}

void vm_init(vm_space_t *vm)
{
    vm_drop_files(vm);
    vm->count = 0;
    vm->heap_start = 0;
    vm->brk = 0;
    vm->stack_limit = 0;
}

void vm_free(vm_space_t *vm)
{
    vm_drop_files(vm);
    kfree(vm->vmas);
    vm->vmas = NULL;
    vm->cap = 0;
    vm->count = 0;
}

int vm_copy(vm_space_t *dst, const vm_space_t *src)
{
    uint32_t i;

    *dst = *src;
    dst->vmas = NULL;
    dst->cap = 0;
    if (src->count == 0) {
        return 0;
    }
    dst->vmas = kmalloc(sizeof(vma_t) * src->cap);
    if (!dst->vmas) {
        dst->count = 0;
        return -1;
    }
    k_memcpy(dst->vmas, src->vmas, sizeof(vma_t) * src->count);
    dst->cap = src->cap;
    for (i = 0; i < dst->count; i++) {
        vm_file_get(dst->vmas[i].file);
    }
    return 0;
}

int vm_reserve(vm_space_t *vm, uint32_t n)
{
    uint32_t cap;
    vma_t *grown;

    if (vm->cap - vm->count >= n) {
        return 0;
    }
    if (vm->count + n > VMA_LIMIT) {
        return -1;
    }
    cap = vm->cap ? vm->cap : VMA_FIRST;
    while (cap < vm->count + n) {
        cap *= 2;
    }
    if (cap > VMA_LIMIT) {
        cap = VMA_LIMIT;
    }
    grown = kmalloc(sizeof(vma_t) * cap);
    if (!grown) {
        return -1;
    }
    if (vm->count) {
        k_memcpy(grown, vm->vmas, sizeof(vma_t) * vm->count);
    }
    kfree(vm->vmas);
    vm->vmas = grown;
    vm->cap = cap;
    return 0;
}

/** The index of the first region that ends above `addr`: the region that
 * holds `addr` if there is one, else the first region above it, else
 * `count`. */
static uint32_t vm_lower(const vm_space_t *vm, uintptr_t addr)
{
    uint32_t lo = 0;
    uint32_t hi = vm->count;

    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;

        if (vm->vmas[mid].end <= addr) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/** Put `r` at index `i`, moving the regions from `i` up; the caller made
 * room. */
static void vm_insert_at(vm_space_t *vm, uint32_t i, const vma_t *r)
{
    if (i < vm->count) {
        k_memmove(&vm->vmas[i + 1], &vm->vmas[i], sizeof(vma_t) * (vm->count - i));
    }
    vm->vmas[i] = *r;
    vm->count++;
}

/** Remove the region at index `i`, dropping its file's reference. */
static void vm_delete_at(vm_space_t *vm, uint32_t i)
{
    vm_file_put(vm->vmas[i].file);
    if (i + 1 < vm->count) {
        k_memmove(&vm->vmas[i], &vm->vmas[i + 1], sizeof(vma_t) * (vm->count - i - 1));
    }
    vm->count--;
}

/** Merge region `i` into its left neighbour when they touch and agree in
 * kind and protection, and map no file or the same file without a gap. */
static void vm_merge_left(vm_space_t *vm, uint32_t i)
{
    vma_t *l;
    vma_t *r;

    if (i == 0 || i >= vm->count) {
        return;
    }
    l = &vm->vmas[i - 1];
    r = &vm->vmas[i];
    if (l->end == r->start && l->prot == r->prot && l->type == r->type && l->file == r->file &&
        (!l->file || l->pgoff + ((l->end - l->start) >> VM_PAGE_SHIFT) == r->pgoff)) {
        l->end = r->end;
        vm_delete_at(vm, i);
    }
}

vma_t *vm_find(vm_space_t *vm, uintptr_t addr)
{
    uint32_t i = vm_lower(vm, addr);

    if (i < vm->count && vm->vmas[i].start <= addr) {
        return &vm->vmas[i];
    }
    return NULL;
}

int vm_overlaps(const vm_space_t *vm, uintptr_t start, uintptr_t end)
{
    uint32_t i = vm_lower(vm, start);

    return i < vm->count && vm->vmas[i].start < end;
}

int vm_insert(vm_space_t *vm, uintptr_t start, uintptr_t end, uint32_t prot, vm_type_t type)
{
    return vm_insert_file(vm, start, end, prot, type, NULL, 0);
}

int vm_insert_file(vm_space_t *vm, uintptr_t start, uintptr_t end, uint32_t prot, vm_type_t type,
                   struct vm_file *file, uint32_t pgoff)
{
    vma_t r;
    uint32_t i;

    if (start >= end) {
        return -1;
    }
    if ((start & VM_PAGE_MASK) != 0 || (end & VM_PAGE_MASK) != 0) {
        return -1;
    }
    if (vm_overlaps(vm, start, end)) {
        return -1;
    }
    if (vm_reserve(vm, 1) != 0) {
        return -1;
    }

    r.start = start;
    r.end = end;
    r.prot = prot;
    r.type = type;
    r.file = file;
    r.pgoff = pgoff;
    vm_file_get(file);
    i = vm_lower(vm, start);
    vm_insert_at(vm, i, &r);
    vm_merge_left(vm, i + 1);
    vm_merge_left(vm, i);
    return 0;
}

/**
 * Only the split needs a new entry, and it is made room for before anything
 * changes. A region whose low end goes is the last the span meets, since
 * the regions are sorted and disjoint.
 */
int vm_remove(vm_space_t *vm, uintptr_t start, uintptr_t end)
{
    uint32_t i;

    if (start >= end) {
        return 0;
    }

    i = vm_lower(vm, start);
    if (i < vm->count && vm->vmas[i].start < start && vm->vmas[i].end > end) {
        vma_t upper;

        if (vm_reserve(vm, 1) != 0) {
            return -1;
        }
        upper = vm_cut(&vm->vmas[i], end);
        vm->vmas[i].end = start;
        vm_insert_at(vm, i + 1, &upper);
        return 0;
    }

    while (i < vm->count && vm->vmas[i].start < end) {
        vma_t *r = &vm->vmas[i];

        if (start <= r->start && end >= r->end) {
            vm_delete_at(vm, i);
        } else if (start <= r->start) {
            r->pgoff += r->file ? (uint32_t)((end - r->start) >> VM_PAGE_SHIFT) : 0u;
            r->start = end;
            break;
        } else {
            r->end = start;
            i++;
        }
    }
    return 0;
}

/**
 * The range is walked for the gap and the refused write before anything
 * changes, and the two splits' entries are made room for first. The merges
 * run from the top down, so that each leaves the lower indices where they
 * were.
 */
int vm_protect(vm_space_t *vm, uintptr_t start, uintptr_t end, uint32_t prot)
{
    uintptr_t at;
    uint32_t i, first;

    if (start >= end) {
        return 0;
    }
    at = start;
    for (i = vm_lower(vm, start); i < vm->count && at < end; i++) {
        if (vm->vmas[i].start > at) {
            break;
        }
        if ((prot & VM_PROT_WRITE) && (vm->vmas[i].prot & VM_PROT_NOWRITE)) {
            return -3;
        }
        at = vm->vmas[i].end;
    }
    if (at < end) {
        return -1;
    }
    if (vm_reserve(vm, 2) != 0) {
        return -2;
    }

    i = vm_lower(vm, start);
    if (vm->vmas[i].start < start) {
        vma_t upper = vm_cut(&vm->vmas[i], start);

        vm_insert_at(vm, i + 1, &upper);
        i++;
    }
    first = i;
    while (i < vm->count && vm->vmas[i].start < end) {
        if (vm->vmas[i].end > end) {
            vma_t upper = vm_cut(&vm->vmas[i], end);

            vm_insert_at(vm, i + 1, &upper);
        }
        vm->vmas[i].prot = prot | (vm->vmas[i].prot & (VM_PROT_NOWRITE | VM_PROT_IO));
        i++;
    }
    if (i < vm->count) {
        vm_merge_left(vm, i);
    }
    while (i > first) {
        i--;
        vm_merge_left(vm, i);
    }
    return 0;
}

uintptr_t vm_find_free(const vm_space_t *vm, uintptr_t len, uintptr_t lo, uintptr_t hi)
{
    uint32_t i;
    uintptr_t top, bottom;

    if (len == 0 || hi <= lo || hi - lo < len) {
        return 0;
    }
    i = vm_lower(vm, hi);
    top = hi;
    if (i < vm->count && vm->vmas[i].start < top) {
        top = vm->vmas[i].start;
    }
    for (;;) {
        bottom = lo;
        if (i > 0 && vm->vmas[i - 1].end > bottom) {
            bottom = vm->vmas[i - 1].end;
        }
        if (top > bottom && top - bottom >= len) {
            return top - len;
        }
        if (i == 0) {
            return 0;
        }
        i--;
        if (vm->vmas[i].start <= lo) {
            return 0;
        }
        top = vm->vmas[i].start;
    }
}

uintptr_t vm_covered(const vm_space_t *vm, uintptr_t start, uintptr_t end)
{
    uintptr_t total = 0;
    uint32_t i;

    for (i = vm_lower(vm, start); i < vm->count && vm->vmas[i].start < end; i++) {
        uintptr_t s = vm->vmas[i].start > start ? vm->vmas[i].start : start;
        uintptr_t e = vm->vmas[i].end < end ? vm->vmas[i].end : end;

        total += e - s;
    }
    return total;
}

uintptr_t vm_total(const vm_space_t *vm)
{
    uintptr_t total = 0;
    uint32_t i;

    for (i = 0; i < vm->count; i++) {
        total += vm->vmas[i].end - vm->vmas[i].start;
    }
    return total;
}

int vm_grow_stack(vm_space_t *vm, uintptr_t page, uintptr_t as_limit)
{
    uint32_t i, j;

    for (i = 0; i < vm->count; i++) {
        vma_t *r = &vm->vmas[i];

        if (r->type == VM_STACK && page < r->start && page >= vm->stack_limit) {
            if (vm_total(vm) + (r->start - page) > as_limit) {
                return 0;
            }
            for (j = 0; j < vm->count; j++) {
                if (j != i && page < vm->vmas[j].end && r->start > vm->vmas[j].start) {
                    return 0;
                }
            }
            r->start = page;
            return 1;
        }
    }
    return 0;
}
