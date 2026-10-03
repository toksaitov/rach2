/*
 * kheap_hosttest.c: the heap's size, its page runs, the reserve for large
 * ones, and the guarded runs kernel stacks come from.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * The page allocator is driven with the frames replaced by a counter and
 * the page tables by a map of the region's pages that have a mapping.
 * Nothing on this path is dereferenced, since a page from `kalloc_pages`
 * has no header, so the addresses are the kernel's own and the arithmetic
 * on the page map is what is tested. The rows of the runs give the heap a
 * small region, 8192 pages, of which the reserve is 4096, with its page map
 * in the image. The region is 260 MiB of host memory, so that the last rows
 * give the heap the whole of it, more than KHEAP_FLOOR, whose page map is
 * then in the region's own first pages. The size the boot gives the heap is
 * checked on i686's region and on x86-64's, at the sizes of memory a machine
 * has: an eighth, the floor and the cap.
 */

extern int printf(const char *fmt, ...);

/* The kernel's integer types, with the pointer-sized one the host's width,
   so that a pointer into the region below survives the round trip through
   it: the region is a static array here, which `kmalloc` writes its headers
   into, and the kernel's 32-bit type would cut its address. */
#define _KINT_H_
typedef signed char int8_t;
typedef unsigned char uint8_t;
typedef signed short int16_t;
typedef unsigned short uint16_t;
typedef signed int int32_t;
typedef unsigned int uint32_t;
typedef signed long long int64_t;
typedef unsigned long long uint64_t;
typedef unsigned long uintptr_t;

#include "../../rach2/sys/lib/kerrno.h"
#include "../../rach2/sys/mm/paging.h"
#undef KVA_HEAP_SIZE
#define KVA_HEAP_SIZE (260u * 1024u * 1024u)
static unsigned char heap_mem[KVA_HEAP_SIZE] __attribute__((aligned(4096)));
#undef KVA_HEAP
#define KVA_HEAP ((uintptr_t)heap_mem)

/* The region the rows of the runs give the heap. */
#define SMALL_REGION (32u * 1024u * 1024u)

/* The regions of the architectures' layouts: i686's whole region, and the
 * PML4 entry of x86-64's (cpu.h). */
#define I686_REGION   0x10000000ULL
#define X86_64_REGION 0x8000000000ULL

/* The frames: a counter of those out, and a limit past which none is
   given, for the row about a shortage inside a run. */
static unsigned frames_out;
static unsigned frame_limit = 0xffffffffu;

/* The region's pages that have a mapping, as the kernel directory's page
   tables would say: the guard rows read it, and `kfree_guarded` asks it
   through `paging_virt_to_phys`. */
static unsigned char mapped[KVA_HEAP_SIZE / PAGE_SIZE];

static unsigned region_page(uintptr_t va)
{
    return (unsigned)((va - KVA_HEAP) / PAGE_SIZE);
}

uintptr_t alloc_frame(void)
{
    if (frames_out >= frame_limit) {
        return 0;
    }
    frames_out++;
    return 0x10000000u + frames_out * PAGE_SIZE;
}

void free_frame(uintptr_t phys)
{
    (void)phys;
    frames_out--;
}

void *alloc_kernel_frame(void)
{
    return (void *)0;
}

uintptr_t paging_virt_to_phys(uintptr_t va)
{
    return mapped[region_page(va)] ? va : 0;
}

void map_kernel_frame(uintptr_t va, uintptr_t phys, int writable)
{
    (void)phys;
    (void)writable;
    mapped[region_page(va)] = 1;
}

void unmap_kernel_frame(uintptr_t va)
{
    mapped[region_page(va)] = 0;
}

/* What `premap_kernel_window` was last asked for: the heap's tables. */
static uintptr_t premapped_va;
static size_t premapped_size;

int premap_kernel_window(uintptr_t va_start, size_t size)
{
    premapped_va = va_start;
    premapped_size = size;
    return 0;
}

static unsigned panics;

void k_panic(const char *message)
{
    (void)message;
    panics++;
}

uint32_t kirq_save(void)
{
    return 0;
}

void kirq_restore(uint32_t flags)
{
    (void)flags;
}

static unsigned klogs;

void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
    klogs++;
}

#include "../../rach2/sys/mm/kheap.c"

static int fails;

#define CHECK(cond, what)                   \
    do {                                    \
        if (!(cond)) {                      \
            fails++;                        \
            printf("  FAIL: %s\n", (what)); \
        }                                   \
    } while (0)

#define REGION_PAGES  (SMALL_REGION / PAGE_SIZE)
#define RESERVE_PAGES (KHEAP_RUNS * KHEAP_RUN_PAGES)
#define RESERVE_BASE  (KVA_HEAP + (REGION_PAGES - RESERVE_PAGES) * PAGE_SIZE)

static void *singles[REGION_PAGES];

/* Take single pages until `kalloc_pages` returns NULL; the count taken. */
static unsigned take_singles(void)
{
    unsigned n = 0;

    while (n < REGION_PAGES) {
        singles[n] = kalloc_pages(1, 0);
        if (!singles[n]) {
            break;
        }
        n++;
    }
    return n;
}

static void give_singles(unsigned n)
{
    while (n > 0) {
        n--;
        if (singles[n]) {
            kfree_pages(singles[n], 1);
            singles[n] = 0;
        }
    }
}

/**
 * The guarded runs a kernel stack comes from. The page below the run is
 * taken and never mapped, takes no frame, and is given to no other
 * allocation while the run is out; the free gives back the run's frames and
 * the guard's place. A shortage of frames inside the run leaves no guard
 * behind and no page of the run mapped, and a run with no guard below it is
 * refused by the guarded free and kept, with the page below it untouched.
 */
static void guard_rows(void)
{
    char *stack, *below, *plain;
    unsigned n, i, guard, hit;

    kheap_init(SMALL_REGION);
    stack = kalloc_guarded(4);
    CHECK(stack != 0 && frames_out == 4, "four guarded pages take four frames and the guard none");
    if (!stack) {
        return;
    }
    guard = region_page((uintptr_t)stack) - 1;
    CHECK(mapped[guard + 1] && mapped[guard + 4] && !mapped[guard],
          "the four pages are mapped and the page below them is not");
    n = take_singles();
    CHECK(n == REGION_PAGES - RESERVE_PAGES - 5,
          "single pages fill the region around the guarded run and leave its guard");
    for (i = 0, hit = 0; i < n; i++) {
        hit += singles[i] == stack - PAGE_SIZE;
    }
    CHECK(hit == 0 && !mapped[guard], "no single page is the guard, and the guard stays unmapped");
    give_singles(n);
    kfree_guarded(stack, 4);
    CHECK(frames_out == 0, "the guarded free gives back the four frames");
    n = take_singles();
    CHECK(n == REGION_PAGES - RESERVE_PAGES, "and the guard's page with them");
    give_singles(n);

    kheap_init(SMALL_REGION);
    for (i = 0, hit = 0; i < REGION_PAGES; i++) {
        hit += mapped[i];
    }
    frame_limit = frames_out + 2;
    klogs = 0;
    CHECK(kalloc_guarded(4) == 0 && frames_out == 0 && klogs == 1,
          "a guarded run the frames cannot fill is refused, with a record and no frame kept");
    frame_limit = 0xffffffffu;
    for (i = 0; i < REGION_PAGES; i++) {
        hit -= mapped[i];
    }
    CHECK(hit == 0, "and unmaps each page it had mapped");
    n = take_singles();
    CHECK(n == REGION_PAGES - RESERVE_PAGES, "and leaves no guard behind");
    give_singles(n);

    kheap_init(SMALL_REGION);
    CHECK(kalloc_guarded(0) == 0 && kalloc_guarded(KHEAP_RUN_LEAST) == 0,
          "a guarded run of no pages, or of a reserve run's size, is refused");

    kheap_init(SMALL_REGION);
    below = kalloc_pages(1, 0);
    plain = kalloc_pages(4, 0);
    klogs = 0;
    kfree_guarded(plain, 4);
    CHECK(below != 0 && plain != 0 && klogs == 1 && frames_out == 5 &&
              mapped[region_page((uintptr_t)below)] && page_taken(region_page((uintptr_t)below)),
          "pages with no guard below them are refused by the guarded free, the page below kept");
    kfree_pages(plain, 4);
    kfree_pages(below, 1);
    CHECK(frames_out == 0, "and every frame went back after the refusal");
}

#define MIB(n) ((uint64_t)(n) * 1024u * 1024u)
#define GIB(n) (MIB(n) * 1024u)

/**
 * The size the boot gives the heap: on i686 the region, 256 MiB, at any
 * memory; on x86-64 an eighth of the memory managed, in grains of 4 MiB,
 * the floor of 256 MiB below 2 GiB, and the PML4 entry's 512 GiB at 4 TiB
 * and above. The 8 GiB machine is QEMU's at -m 8192, whose boot manages
 * 8362944 KiB.
 */
static void size_rows(void)
{
    CHECK(kheap_size_for(MIB(512), I686_REGION) == MIB(256), "i686, 512 MiB: the region");
    CHECK(kheap_size_for(GIB(4), I686_REGION) == MIB(256), "i686, 4 GiB: the region");
    CHECK(kheap_size_for(MIB(512), X86_64_REGION) == MIB(256), "x86-64, 512 MiB: the floor");
    CHECK(kheap_size_for(GIB(2) - 1, X86_64_REGION) == MIB(256), "x86-64, under 2 GiB: the floor");
    CHECK(kheap_size_for(GIB(3), X86_64_REGION) == MIB(384), "x86-64, 3 GiB: an eighth");
    CHECK(kheap_size_for(8362944ULL * 1024u, X86_64_REGION) == MIB(1020),
          "x86-64, QEMU's 8 GiB: an eighth in grains of 4 MiB");
    CHECK(kheap_size_for(GIB(64), X86_64_REGION) == GIB(8), "x86-64, 64 GiB: an eighth");
    CHECK(kheap_size_for(GIB(4096) - MIB(1), X86_64_REGION) == GIB(512) - MIB(4),
          "x86-64, just under 4 TiB: an eighth, under the cap");
    CHECK(kheap_size_for(GIB(4096), X86_64_REGION) == GIB(512), "x86-64, 4 TiB: the cap");
    CHECK(kheap_size_for(GIB(64 * 1024), X86_64_REGION) == GIB(512), "x86-64, 64 TiB: the cap");
    CHECK(kheap_size_for(GIB(64), MIB(64)) == MIB(64), "a region under the floor: the region");
}

/**
 * A heap of the whole 260 MiB, more than the floor: its tables are made for
 * the region it has and no more, its page map takes the region's first
 * three pages, mapped, taken and held, the first allocation comes after
 * them, the reserve is the region's last 16 MiB, and the heap's size and
 * its large objects follow the region.
 */
static void region_rows(void)
{
    uint32_t pages = KVA_HEAP_SIZE / PAGE_SIZE;
    uint32_t held = 0;
    size_t used = 0;
    void *one, *run, *big;

    frames_out = 0;
    panics = 0;
    kheap_init(KVA_HEAP_SIZE);
    CHECK(panics == 0, "a heap past the floor starts");
    CHECK(premapped_va == KVA_HEAP && premapped_size == KVA_HEAP_SIZE,
          "with the tables of its region made, and no more");
    CHECK(kheap_region_pages() == pages, "and the region's pages counted");
    CHECK(page_bits == (uint32_t *)KVA_HEAP, "its page map is the region's first page");
    CHECK(frames_out == 3 && mapped[0] && mapped[2] && !mapped[3],
          "of three pages, a frame each, for 66560 pages");
    CHECK(page_taken(0) && page_taken(2) && !page_taken(3), "and taken");
    kheap_stats(&held, &used);
    CHECK(held == 3 && used == 0, "and held by the heap, with nothing in use");
    one = kalloc_pages(1, 0);
    CHECK(one == (void *)(KVA_HEAP + 3u * PAGE_SIZE), "the first page given is after the map");
    CHECK(page_taken(pages - RESERVE_PAGES) && !page_taken(pages - RESERVE_PAGES - 1),
          "the reserve is the region's last 16 MiB");
    run = kalloc_pages(pages - RESERVE_PAGES - 4, 0);
    CHECK(run != 0 && (uintptr_t)run + (pages - RESERVE_PAGES - 4) * (uintptr_t)PAGE_SIZE ==
                          KVA_HEAP + (uintptr_t)(pages - RESERVE_PAGES) * PAGE_SIZE,
          "every page below the reserve but the map's is one run");
    big = kmalloc(KHEAP_RUN_PAGES * PAGE_SIZE - KHEAP_HDR);
    CHECK(big != 0 && (uintptr_t)big >= KVA_HEAP + (uintptr_t)(pages - RESERVE_PAGES) * PAGE_SIZE,
          "and a large object then comes from the reserve");
    kheap_stats(&held, &used);
    CHECK(used == KHEAP_RUN_PAGES * PAGE_SIZE - KHEAP_HDR, "counted in the bytes in use");
    kfree(big);
    kfree_pages(run, pages - RESERVE_PAGES - 4);
    kfree_pages(one, 1);
    CHECK(frames_out == 3, "and every frame but the map's went back");
    CHECK(kmalloc((size_t)KVA_HEAP_SIZE) == 0, "an object the size of the region is refused");
}

int main(void)
{
    unsigned n, i, k;
    void *runs[KHEAP_RUNS + 1];

    /* The reserve is closed to single pages from the start: the region
       gives every page below it and none of it. */
    kheap_init(SMALL_REGION);
    n = take_singles();
    CHECK(n == REGION_PAGES - RESERVE_PAGES,
          "single pages fill the region up to the reserve and no further");
    give_singles(n);
    CHECK(frames_out == 0, "and every frame went back");

    /* The free pages scattered as a cache leaves them, one in two: no run
       of two remains below the reserve, and a run of 256 pages comes from
       the reserve, sixteen times, the seventeenth refused; one given back
       lets another come. */
    kheap_init(SMALL_REGION);
    n = take_singles();
    for (i = 0; i < n; i += 2) {
        kfree_pages(singles[i], 1);
        singles[i] = 0;
    }
    CHECK(kalloc_pages(2, 0) == 0, "no run of two remains among the scattered pages");
    for (k = 0; k <= KHEAP_RUNS; k++) {
        runs[k] = kalloc_pages(KHEAP_RUN_PAGES, 0);
    }
    for (k = 0; k < KHEAP_RUNS; k++) {
        unsigned j;

        CHECK(runs[k] != 0, "a run of 256 pages comes from the reserve beside the scatter");
        CHECK(runs[k] == 0 || (uintptr_t)runs[k] >= RESERVE_BASE, "and lies in the reserve");
        for (j = 0; j < k; j++) {
            uintptr_t a = (uintptr_t)runs[k], b = (uintptr_t)runs[j];

            CHECK(a >= b + KHEAP_RUN_PAGES * PAGE_SIZE || b >= a + KHEAP_RUN_PAGES * PAGE_SIZE,
                  "and apart from every other run");
        }
    }
    CHECK(runs[KHEAP_RUNS] == 0, "the seventeenth run is refused");
    kfree_pages(runs[3], KHEAP_RUN_PAGES);
    runs[3] = kalloc_pages(KHEAP_RUN_PAGES, 0);
    CHECK(runs[3] != 0 && (uintptr_t)runs[3] >= RESERVE_BASE, "a run given back is given again");
    for (k = 0; k < KHEAP_RUNS; k++) {
        kfree_pages(runs[k], KHEAP_RUN_PAGES);
    }
    /* A run given back keeps the reserve closed to single pages. */
    for (i = 0; i < n; i += 2) {
        singles[i] = kalloc_pages(1, 0);
    }
    CHECK(kalloc_pages(1, 0) == 0,
          "single pages still do not enter the reserve after its runs came back");
    give_singles(n);
    CHECK(frames_out == 0, "and every frame went back after the scatter");

    /* A large object of `kmalloc` beside the scatter comes from the reserve
       too, and its `kfree` gives the run back and keeps the reserve closed:
       the two ways pages leave the heap agree. */
    kheap_init(SMALL_REGION);
    n = take_singles();
    for (i = 0; i < n; i += 2) {
        kfree_pages(singles[i], 1);
        singles[i] = 0;
    }
    {
        void *big = kmalloc(KHEAP_RUN_LEAST * PAGE_SIZE);

        CHECK(big != 0 && (uintptr_t)big >= RESERVE_BASE,
              "a large object beside the scatter comes from the reserve");
        CHECK(run_bits != 0, "taking a run");
        kfree(big);
        CHECK(run_bits == 0, "and its free gives the run back");
        for (i = 0; i < n; i += 2) {
            singles[i] = kalloc_pages(1, 0);
        }
        CHECK(kalloc_pages(1, 0) == 0, "with the reserve still closed to single pages");
    }
    give_singles(n);
    CHECK(frames_out == 0, "and every frame went back after the large object");

    /* A refusal is logged by `kalloc_pages` and not by `kalloc_pages_try`,
       whose callers go on with less. */
    kheap_init(SMALL_REGION);
    n = take_singles();
    klogs = 0;
    CHECK(kalloc_pages(1, 0) == 0 && klogs == 1, "a refusal is logged once");
    CHECK(kalloc_pages_try(1, 0) == 0 && klogs == 1, "and not by the quiet form");
    give_singles(n);

    /* With room below the reserve, a large run is taken there and the
       reserve stays whole. */
    kheap_init(SMALL_REGION);
    runs[0] = kalloc_pages(KHEAP_RUN_PAGES, 0);
    CHECK(runs[0] != 0 && (uintptr_t)runs[0] < RESERVE_BASE,
          "a run with room below the reserve is taken below it");
    CHECK(run_bits == 0, "and the reserve stays whole");
    kfree_pages(runs[0], KHEAP_RUN_PAGES);

    /* A shortage of frames inside a reserve run gives the run back: the
       allocation is refused, and once frames are there again the same run
       serves. */
    kheap_init(SMALL_REGION);
    n = take_singles();
    for (i = 0; i < n; i += 2) {
        kfree_pages(singles[i], 1);
        singles[i] = 0;
    }
    frame_limit = frames_out + 100;
    CHECK(kalloc_pages(KHEAP_RUN_PAGES, 0) == 0, "a run the frames cannot fill is refused");
    CHECK(run_bits == 0, "and its run is given back");
    frame_limit = 0xffffffffu;
    runs[0] = kalloc_pages(KHEAP_RUN_PAGES, 0);
    CHECK(runs[0] != 0 && (uintptr_t)runs[0] >= RESERVE_BASE,
          "and comes once the frames are there");
    kfree_pages(runs[0], KHEAP_RUN_PAGES);
    give_singles(n);
    CHECK(frames_out == 0, "and every frame went back after the shortage");
    guard_rows();
    size_rows();
    region_rows();
    if (fails == 0) {
        printf("kheap_hosttest: PASS\n");
    }
    return fails > 255 ? 255 : fails;
}
