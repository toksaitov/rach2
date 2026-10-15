/**
 * Host test of the regions of an address space, mm/vm.c, run under the
 * sanitizers.
 *
 * host-deps: rach2/sys/mm/vm.c
 *
 * The rows check an insert, which fails for an empty, unaligned or
 * overlapping range; the search by address and the test of overlap; the
 * four cases of munmap (full cover, left trim, right trim, middle split);
 * the growth of a stack; the array's growth past what it starts with; the
 * merging of neighbours; mprotect's splits and merges, and its failure for
 * a gap and for write to a region that may never be written, which keeps
 * that mark; the search for a free range; the bytes a range covers; and the
 * file a region maps: its pages through splits and merges, its references,
 * and its release, which waits for vm_file_reap after the last one goes.
 */

#include "mm/vm.h"

/* The host's allocator stands in for the kernel heap; declared here, since
 * the kernel's headers define the types the host's stdlib.h would define
 * again. */
void *malloc(unsigned long n);
void free(void *p);

/** The host's malloc. */
void *kmalloc(size_t n)
{
    return malloc(n);
}

/** The host's free. */
void kfree(void *p)
{
    free(p);
}

/** Copy `n` bytes from `src` to `dest`; `dest`. */
void *k_memcpy(void *dest, const void *src, size_t n)
{
    unsigned char *d = dest;
    const unsigned char *s = src;

    while (n-- > 0) {
        *d++ = *s++;
    }
    return dest;
}

/** Copy `n` bytes from `src` to `dest`, which may overlap; `dest`. */
void *k_memmove(void *dest, const void *src, size_t n)
{
    unsigned char *d = dest;
    const unsigned char *s = src;

    if (d < s) {
        while (n-- > 0) {
            *d++ = *s++;
        }
    } else {
        while (n-- > 0) {
            d[n] = s[n];
        }
    }
    return dest;
}

static int fails;

/* A mapped file that counts its releases, for the cases below. */
static int released;

/** A fill that leaves the frame as it is; 0. */
static int no_fill(struct vm_file *f, uint32_t pgoff, uintptr_t phys)
{
    (void)f;
    (void)pgoff;
    (void)phys;
    return 0;
}

/** Count a release of the file. */
static void count_release(struct vm_file *f)
{
    (void)f;
    released++;
}

#define CHECK(cond)    \
    do {               \
        if (!(cond)) { \
            fails++;   \
        }              \
    } while (0)

#define PG 0x1000u

/** A region that may never be written keeps VM_PROT_NOWRITE through
 * mprotect's splits and merges, and mprotect for write over a range that
 * reaches it fails, changing nothing, as it does for a gap. */
static void nowrite_rows(void)
{
    vm_space_t p = {0};
    vma_t *r;

    vm_init(&p);
    CHECK(vm_insert(&p, 0x60000, 0x62000, VM_PROT_READ | VM_PROT_WRITE, VM_ANON) == 0);
    CHECK(vm_insert(&p, 0x62000, 0x66000, VM_PROT_READ | VM_PROT_NOWRITE, VM_SHARED) == 0);
    CHECK(vm_protect(&p, 0x60000, 0x64000, VM_PROT_READ | VM_PROT_WRITE) == -3);
    CHECK(p.count == 2 && p.vmas[0].prot == (VM_PROT_READ | VM_PROT_WRITE) &&
          p.vmas[1].start == 0x62000 && p.vmas[1].prot == (VM_PROT_READ | VM_PROT_NOWRITE));
    CHECK(vm_protect(&p, 0x63000, 0x64000, 0) == 0 && p.count == 4);
    r = vm_find(&p, 0x63000);
    CHECK(r != NULL && r->start == 0x63000 && r->end == 0x64000 && r->prot == VM_PROT_NOWRITE);
    CHECK(vm_protect(&p, 0x63000, 0x64000, VM_PROT_READ) == 0);
    CHECK(p.count == 2 && p.vmas[1].start == 0x62000 && p.vmas[1].end == 0x66000 &&
          p.vmas[1].prot == (VM_PROT_READ | VM_PROT_NOWRITE));
    vm_free(&p);
}

/**
 * A region that maps a file: a split gives the upper part the file's later
 * pages and a reference of its own; the pieces join again only where the
 * pages continue, and regions that meet in the address space with pages
 * that do not continue stay apart; a copy for fork takes references of its
 * own, and the file waits for vm_file_reap after its last reference goes.
 */
static void file_rows(void)
{
    vm_space_t m = {0};
    vm_space_t c = {0};
    struct vm_file file = {1, no_fill, NULL, count_release};
    vma_t *lo, *hi;

    released = 0;
    CHECK(vm_insert_file(&m, 0x40000, 0x48000, VM_PROT_READ, VM_DATA, &file, 3) == 0);
    CHECK(file.refs == 2);
    CHECK(vm_protect(&m, 0x42000, 0x44000, VM_PROT_READ | VM_PROT_WRITE) == 0);
    CHECK(m.count == 3 && file.refs == 4);
    lo = vm_find(&m, 0x42000);
    hi = vm_find(&m, 0x44000);
    CHECK(lo != NULL && lo->pgoff == 5 && hi != NULL && hi->pgoff == 7);
    CHECK(vm_protect(&m, 0x42000, 0x44000, VM_PROT_READ) == 0);
    CHECK(m.count == 1 && file.refs == 2 && m.vmas[0].pgoff == 3);
    CHECK(vm_remove(&m, 0x41000, 0x42000) == 0);
    CHECK(m.count == 2 && file.refs == 3);
    hi = vm_find(&m, 0x42000);
    CHECK(hi != NULL && hi->start == 0x42000 && hi->pgoff == 5);
    CHECK(vm_remove(&m, 0x42000, 0x43000) == 0);
    hi = vm_find(&m, 0x43000);
    CHECK(hi != NULL && hi->pgoff == 6);
    /* Page 4 at 0x41000 does not join the region above, at page 6. */
    CHECK(vm_insert_file(&m, 0x41000, 0x42000, VM_PROT_READ, VM_DATA, &file, 4) == 0);
    CHECK(m.count == 2 && m.vmas[0].end == 0x42000);
    /* Page 9 at 0x42000 continues neither neighbour and stays apart. */
    CHECK(vm_insert_file(&m, 0x42000, 0x43000, VM_PROT_READ, VM_DATA, &file, 9) == 0);
    CHECK(m.count == 3 && m.vmas[1].start == 0x42000 && m.vmas[1].end == 0x43000 &&
          m.vmas[1].pgoff == 9 && file.refs == 4);
    CHECK(vm_remove(&m, 0x42000, 0x43000) == 0 && m.count == 2 && file.refs == 3);
    CHECK(vm_copy(&c, &m) == 0 && file.refs == 5);
    vm_free(&c);
    CHECK(file.refs == 3 && released == 0);
    vm_free(&m);
    CHECK(file.refs == 1 && released == 0);
    vm_file_put(&file);
    CHECK(file.refs == 0 && released == 0);
    vm_file_reap();
    CHECK(released == 1);
}

/** The last region to let go of a file leaves it for vm_file_reap, which
 * releases each file it finds once: a removal must not sleep in the
 * release while the regions are half changed. */
static void reap_rows(void)
{
    vm_space_t m = {0};
    struct vm_file one = {1, no_fill, NULL, count_release};
    struct vm_file two = {1, no_fill, NULL, count_release};

    released = 0;
    CHECK(vm_insert_file(&m, 0x40000, 0x42000, VM_PROT_READ, VM_FILE, &one, 0) == 0);
    CHECK(vm_insert_file(&m, 0x42000, 0x43000, VM_PROT_READ, VM_FILE, &two, 0) == 0);
    vm_file_put(&one);
    vm_file_put(&two);
    CHECK(vm_remove(&m, 0x40000, 0x43000) == 0 && m.count == 0 && released == 0);
    vm_file_reap();
    CHECK(released == 2 && one.refs == 0 && two.refs == 0);
    vm_file_reap();
    CHECK(released == 2);
    vm_free(&m);
}

/**
 * Run every row; the exit status is the count of failed checks, at most 100.
 *
 * A stack grows down to a page below it at or above its limit, and the
 * pages between come into the region, to fault in later. A page at or above
 * the stack's start is no growth, and a space with no stack has none to
 * grow. A stack does not grow onto or past another region below it, and its
 * growth counts against an address-space limit.
 *
 * mprotect cuts a region in three and merges it back, changes both regions
 * a range covers, and fails for a range with a gap, changing nothing. The
 * free range found is the highest that fits below hi and at or above lo; a
 * hole a removal left is used again, and 0 is returned when nothing fits.
 */
int main(void)
{
    vm_space_t vm = {0};
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

    /* Middle split: a hole in a region of four pages leaves two regions. */
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

    /* Stack growth: a stack of one page at the top, its limit below. */
    {
        vm_space_t s = {0};
        vma_t *st;

        vm_init(&s);
        s.stack_limit = 0xBF7FF000u;
        CHECK(vm_insert(&s, 0xBFFFF000u, 0xC0000000u, VM_PROT_READ | VM_PROT_WRITE, VM_STACK) == 0);

        /* A page just below the stack grows it down by one page. */
        CHECK(vm_grow_stack(&s, 0xBFFFE000u, 0xFFFFFFFFu) == 1);
        st = vm_find(&s, 0xBFFFE000u);
        CHECK(st != NULL && st->start == 0xBFFFE000u && st->end == 0xC0000000u);
        CHECK(s.count == 1); /* grown in place, not a new region */

        /* A page far below, above the limit, takes the start down to it. */
        CHECK(vm_grow_stack(&s, 0xBF800000u, 0xFFFFFFFFu) == 1);
        CHECK(vm_find(&s, 0xBF800000u) != NULL);
        CHECK(vm_find(&s, 0xBFF00000u) != NULL); /* a page of the gap, in the region */

        /* Growth to the limit succeeds, and below it fails. */
        CHECK(vm_grow_stack(&s, 0xBF7FF000u, 0xFFFFFFFFu) == 1);
        CHECK(vm_grow_stack(&s, 0xBF7FE000u, 0xFFFFFFFFu) == 0);
        CHECK(vm_find(&s, 0xBF7FF000u) != NULL);
        CHECK(vm_find(&s, 0xBF7FE000u) == NULL);

        /* A page at or above the stack's start is no growth. */
        CHECK(vm_grow_stack(&s, 0xBF7FF000u, 0xFFFFFFFFu) == 0); /* the stack's start */

        /* With no stack region, growth always fails. */
        vm_init(&s);
        CHECK(vm_grow_stack(&s, 0xBFFFE000u, 0xFFFFFFFFu) == 0);

        /* Another region below the stack stops its growth. */
        vm_init(&s);
        s.stack_limit = 0xBF7FF000u;
        CHECK(vm_insert(&s, 0xBFFFF000u, 0xC0000000u, VM_PROT_READ | VM_PROT_WRITE, VM_STACK) == 0);
        CHECK(vm_insert(&s, 0xBF900000u, 0xBF901000u, VM_PROT_READ, VM_DATA) == 0);
        /* One page down is still clear: allowed. */
        CHECK(vm_grow_stack(&s, 0xBFFFE000u, 0xFFFFFFFFu) == 1);
        /* Growth onto or past that region fails, and the stack stays. */
        CHECK(vm_grow_stack(&s, 0xBF900000u, 0xFFFFFFFFu) == 0);
        CHECK(vm_grow_stack(&s, 0xBF8FF000u, 0xFFFFFFFFu) == 0);
        CHECK(vm_find(&s, 0xBFFFE000u) != NULL && vm_find(&s, 0xBF900000u) != NULL);
        CHECK(vm_find(&s, 0xBF8FF000u) == NULL); /* stack did not jump the region */
        /* With three pages mapped, a limit of four allows one page more. */
        CHECK(vm_total(&s) == 3 * PG);
        CHECK(vm_grow_stack(&s, 0xBFFFD000u, 4 * PG) == 1);
        CHECK(vm_grow_stack(&s, 0xBFFFC000u, 4 * PG) == 0);
        vm_free(&s);
    }

    /* The array grows to three times its first size; a copy has its own. */
    {
        vm_space_t g = {0};
        vm_space_t c = {0};
        uint32_t i;

        vm_init(&g);
        for (i = 0; i < 3 * VMA_FIRST; i++) {
            CHECK(vm_insert(&g, 0x10000000u + i * 2 * PG, 0x10000000u + i * 2 * PG + PG,
                            VM_PROT_READ, VM_ANON) == 0);
        }
        CHECK(g.count == 3 * VMA_FIRST && g.cap >= 3 * VMA_FIRST);
        for (i = 0; i < 3 * VMA_FIRST; i++) {
            CHECK(vm_find(&g, 0x10000000u + i * 2 * PG) != NULL);
            CHECK(vm_find(&g, 0x10000000u + i * 2 * PG + PG) == NULL);
        }
        CHECK(vm_copy(&c, &g) == 0);
        CHECK(c.count == g.count && c.vmas != g.vmas);
        CHECK(vm_find(&c, 0x10000000u + 40 * 2 * PG) != NULL);
        /* A removal from the copy leaves the original. */
        CHECK(vm_remove(&c, 0x10000000u, 0x10000000u + PG) == 0);
        CHECK(c.count == g.count - 1 && vm_find(&g, 0x10000000u) != NULL);
        vm_free(&c);
        vm_free(&g);
    }
    vm_free(&vm);

    /* Neighbours of one kind and protection merge; the array stays sorted. */
    {
        vm_space_t m = {0};

        vm_init(&m);
        CHECK(vm_insert(&m, 0x30000, 0x31000, VM_PROT_READ, VM_ANON) == 0);
        CHECK(vm_insert(&m, 0x32000, 0x33000, VM_PROT_READ, VM_ANON) == 0);
        CHECK(vm_insert(&m, 0x31000, 0x32000, VM_PROT_READ, VM_ANON) == 0);
        CHECK(m.count == 1 && m.vmas[0].start == 0x30000 && m.vmas[0].end == 0x33000);
        CHECK(vm_insert(&m, 0x33000, 0x34000, VM_PROT_READ | VM_PROT_WRITE, VM_ANON) == 0);
        CHECK(vm_insert(&m, 0x20000, 0x21000, VM_PROT_READ, VM_HEAP) == 0);
        CHECK(m.count == 3 && m.vmas[0].start == 0x20000 && m.vmas[2].start == 0x33000);
        vm_free(&m);
    }

    /* mprotect. */
    {
        vm_space_t p = {0};
        vma_t *r;

        vm_init(&p);
        CHECK(vm_insert(&p, 0x50000, 0x58000, VM_PROT_READ | VM_PROT_WRITE, VM_ANON) == 0);
        CHECK(vm_protect(&p, 0x52000, 0x54000, VM_PROT_READ) == 0);
        CHECK(p.count == 3);
        r = vm_find(&p, 0x52000);
        CHECK(r != NULL && r->start == 0x52000 && r->end == 0x54000 && r->prot == VM_PROT_READ);
        r = vm_find(&p, 0x54000);
        CHECK(r != NULL && r->start == 0x54000 && r->prot == (VM_PROT_READ | VM_PROT_WRITE));
        CHECK(vm_protect(&p, 0x52000, 0x54000, VM_PROT_READ | VM_PROT_WRITE) == 0);
        CHECK(p.count == 1 && p.vmas[0].start == 0x50000 && p.vmas[0].end == 0x58000);
        CHECK(vm_protect(&p, 0x57000, 0x58000, 0) == 0);
        CHECK(vm_insert(&p, 0x58000, 0x5A000, VM_PROT_READ, VM_ANON) == 0);
        CHECK(vm_protect(&p, 0x56000, 0x59000, VM_PROT_READ) == 0);
        r = vm_find(&p, 0x56000);
        CHECK(r != NULL && r->start == 0x56000 && r->end == 0x5A000 && r->prot == VM_PROT_READ);
        CHECK(vm_insert(&p, 0x5B000, 0x5C000, VM_PROT_READ, VM_ANON) == 0);
        CHECK(vm_protect(&p, 0x59000, 0x5C000, 0) == -1); /* 0x5A000 is a gap */
        r = vm_find(&p, 0x59000);
        CHECK(r != NULL && r->prot == VM_PROT_READ);
        CHECK(vm_protect(&p, 0x40000, 0x41000, 0) == -1); /* nothing mapped */
        vm_free(&p);
    }

    /* The free range. */
    {
        vm_space_t f = {0};

        vm_init(&f);
        CHECK(vm_find_free(&f, 2 * PG, 0x10000, 0x20000) == 0x1E000);
        CHECK(vm_insert(&f, 0x1C000, 0x20000, VM_PROT_READ, VM_ANON) == 0);
        CHECK(vm_insert(&f, 0x14000, 0x18000, VM_PROT_READ, VM_HEAP) == 0);
        CHECK(vm_find_free(&f, 2 * PG, 0x10000, 0x20000) == 0x1A000);
        CHECK(vm_find_free(&f, 5 * PG, 0x10000, 0x20000) == 0);
        CHECK(vm_find_free(&f, 4 * PG, 0x10000, 0x20000) == 0x18000);
        CHECK(vm_insert(&f, 0x18000, 0x19000, VM_PROT_READ, VM_ANON) == 0);
        CHECK(vm_find_free(&f, 4 * PG, 0x10000, 0x20000) == 0x10000);
        CHECK(vm_remove(&f, 0x18000, 0x19000) == 0);
        CHECK(vm_remove(&f, 0x1D000, 0x1F000) == 0);
        CHECK(vm_find_free(&f, 2 * PG, 0x19000, 0x20000) == 0x1D000);
        CHECK(vm_covered(&f, 0x13000, 0x1E000) == 4 * PG + PG);
        vm_free(&f);
    }

    nowrite_rows();
    file_rows();
    reap_rows();
    return (fails > 100) ? 100 : fails;
}
