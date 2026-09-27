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

/* Drop the file references of every region, which then go. */
static void vm_drop_files(vm_space_t *vm)
{
    uint32_t i;

    for (i = 0; i < vm->count; i++) {
        vm_file_put(vm->vmas[i].file);
        vm->vmas[i].file = NULL;
    }
}

/* Cut region `r` at `at`, inside it: the part from `at` up, which the caller
 * places, maps the file on from its own page and holds a reference of its
 * own. */
static vma_t vm_cut(vma_t *r, uint32_t at)
{
    vma_t upper = *r;

    upper.start = at;
    if (upper.file) {
        upper.pgoff += (at - r->start) >> VM_PAGE_SHIFT;
        vm_file_get(upper.file);
    }
    r->end = at;
    return upper;
}

/**
 * Reset an address space to empty.
 *
 * The count goes to zero, the array it has is kept for reuse, and the heap,
 * brk and stack limit are zeroed. The space owns no mappings until vm_insert
 * adds them.
 */
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

/* The index of the first region that ends above addr: the region holding
 * addr if there is one, else the first region above it, else count. */
static uint32_t vm_lower(const vm_space_t *vm, uint32_t addr)
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

/* Put r at index i, moving the regions from i up; the caller made room. */
static void vm_insert_at(vm_space_t *vm, uint32_t i, const vma_t *r)
{
    if (i < vm->count) {
        k_memmove(&vm->vmas[i + 1], &vm->vmas[i], sizeof(vma_t) * (vm->count - i));
    }
    vm->vmas[i] = *r;
    vm->count++;
}

static void vm_delete_at(vm_space_t *vm, uint32_t i)
{
    vm_file_put(vm->vmas[i].file);
    if (i + 1 < vm->count) {
        k_memmove(&vm->vmas[i], &vm->vmas[i + 1], sizeof(vma_t) * (vm->count - i - 1));
    }
    vm->count--;
}

/* Merge region i into its left neighbour when they touch and agree in kind
 * and protection, and map no file or the same file without a gap. */
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

/**
 * Return the VMA containing addr, or NULL if none does.
 *
 * Regions are half-open [start, end), so end is not part of the region.
 */
vma_t *vm_find(vm_space_t *vm, uint32_t addr)
{
    uint32_t i = vm_lower(vm, addr);

    if (i < vm->count && vm->vmas[i].start <= addr) {
        return &vm->vmas[i];
    }
    return NULL;
}

/**
 * Report whether [start, end) intersects any existing region.
 */
int vm_overlaps(const vm_space_t *vm, uint32_t start, uint32_t end)
{
    uint32_t i = vm_lower(vm, start);

    return i < vm->count && vm->vmas[i].start < end;
}

/**
 * Add region [start, end) with the given protection and type.
 *
 * The range must be non-empty, page-aligned at both ends, and clear of every
 * existing region, and the array must have room or be able to grow below
 * `VMA_LIMIT`. The region is merged with a neighbour it touches that has the
 * same kind and protection. Returns 0 on success or -1 if any of those
 * conditions fails.
 */
int vm_insert(vm_space_t *vm, uint32_t start, uint32_t end, uint32_t prot, vm_type_t type)
{
    return vm_insert_file(vm, start, end, prot, type, NULL, 0);
}

int vm_insert_file(vm_space_t *vm, uint32_t start, uint32_t end, uint32_t prot, vm_type_t type,
                   struct vm_file *file, uint32_t pgoff)
{
    vma_t r;
    uint32_t i;

    if (start >= end) {
        return -1; /* empty or inverted range */
    }
    if ((start & VM_PAGE_MASK) != 0 || (end & VM_PAGE_MASK) != 0) {
        return -1; /* boundaries must be page-aligned */
    }
    if (vm_overlaps(vm, start, end)) {
        return -1; /* would overlap an existing region */
    }
    if (vm_reserve(vm, 1) != 0) {
        return -1; /* the limit, or no memory */
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
 * Remove the span [start, end) from every region it touches.
 *
 * A fully covered region is dropped, a region clipped at one end is trimmed,
 * and a region straddling the span is split in two. Only the split needs a
 * new entry, and it is made room for before anything changes, so a -1 leaves
 * the regions as they were.
 */
int vm_remove(vm_space_t *vm, uint32_t start, uint32_t end)
{
    uint32_t i;

    if (start >= end) {
        return 0; /* nothing to remove */
    }

    i = vm_lower(vm, start);
    if (i < vm->count && vm->vmas[i].start < start && vm->vmas[i].end > end) {
        vma_t upper;

        if (vm_reserve(vm, 1) != 0) {
            return -1; /* no room for the upper half */
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
            /* The low end goes; nothing above reaches back. */
            r->pgoff += r->file ? (end - r->start) >> VM_PAGE_SHIFT : 0u;
            r->start = end;
            break;
        } else {
            r->end = start; /* the high end goes */
            i++;
        }
    }
    return 0;
}

/**
 * Give [start, end) a new protection.
 *
 * Every page of the range must lie in a region: a gap returns -1, as Linux's
 * mprotect answers ENOMEM. Write is refused with -3 when a region of the
 * range may never be written (VM_PROT_NOWRITE), as Linux answers EACCES; the
 * range is walked for both before anything changes, and the first in address
 * order is the answer. The regions the range cuts are split at its two ends,
 * which needs up to two entries, made room for first; then the pieces inside
 * take `prot`, each keeping its own VM_PROT_NOWRITE, and merge with
 * neighbours that now match. Returns 0, -1 for a gap, -2 when the split has
 * no room, or -3; every error leaves the regions as they were.
 */
int vm_protect(vm_space_t *vm, uint32_t start, uint32_t end, uint32_t prot)
{
    uint32_t i, at;

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
    at = i;
    while (i < vm->count && vm->vmas[i].start < end) {
        if (vm->vmas[i].end > end) {
            vma_t upper = vm_cut(&vm->vmas[i], end);

            vm_insert_at(vm, i + 1, &upper);
        }
        vm->vmas[i].prot = prot | (vm->vmas[i].prot & VM_PROT_NOWRITE);
        i++;
    }
    /* Merge from the top down so each merge leaves the lower indices where
     * they were. */
    if (i < vm->count) {
        vm_merge_left(vm, i);
    }
    while (i > at) {
        i--;
        vm_merge_left(vm, i);
    }
    return 0;
}

uint32_t vm_find_free(const vm_space_t *vm, uint32_t len, uint32_t lo, uint32_t hi)
{
    uint32_t i, top, bottom;

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

uint32_t vm_covered(const vm_space_t *vm, uint32_t start, uint32_t end)
{
    uint32_t i, total = 0;

    for (i = vm_lower(vm, start); i < vm->count && vm->vmas[i].start < end; i++) {
        uint32_t s = vm->vmas[i].start > start ? vm->vmas[i].start : start;
        uint32_t e = vm->vmas[i].end < end ? vm->vmas[i].end : end;

        total += e - s;
    }
    return total;
}

/** Bytes the regions cover together, what RLIMIT_AS is measured against. */
uint32_t vm_total(const vm_space_t *vm)
{
    uint32_t i, total = 0;

    for (i = 0; i < vm->count; i++) {
        total += vm->vmas[i].end - vm->vmas[i].start;
    }
    return total;
}

/**
 * Extend a stack region downward to cover the faulting page.
 *
 * Returns 1 if a stack region was grown to include `page`, 0 otherwise. The
 * page must lie below a VM_STACK region's start and at or above the stack
 * limit set when the stack was made; the region then starts at `page`, and
 * the pages between fault in lazily as the stack touches them. The growth
 * counts against `as_limit` (RLIMIT_AS) as a mapping does, and the new span
 * must be clear of every other region: a hostile ELF segment or mmap
 * inside the growth window must not be absorbed into the stack.
 */
int vm_grow_stack(vm_space_t *vm, uint32_t page, uint32_t as_limit)
{
    uint32_t i, j;

    for (i = 0; i < vm->count; i++) {
        vma_t *r = &vm->vmas[i];

        if (r->type == VM_STACK && page < r->start && page >= vm->stack_limit) {
            /* The growth counts against the address-space limit like a
               mapping would. */
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
