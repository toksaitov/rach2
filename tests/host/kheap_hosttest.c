/*
 * kheap_hosttest.c: the heap's page runs and the reserve for large ones.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * The page allocator is driven with the frames and the page tables replaced
 * by counters. Nothing on this path is dereferenced, since a page from
 * `kalloc_pages` has no header, so the addresses are the kernel's own and
 * the arithmetic on the page map is what is tested. The region is made
 * small, 8192 pages, of which the reserve is 4096.
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
#define KVA_HEAP_SIZE (32u * 1024u * 1024u)
static unsigned char heap_mem[KVA_HEAP_SIZE] __attribute__((aligned(4096)));
#undef KVA_HEAP
#define KVA_HEAP ((uintptr_t)heap_mem)

/* The frames: a counter of those out, and a limit past which none is
   given, for the row about a shortage inside a run. */
static unsigned frames_out;
static unsigned frame_limit = 0xffffffffu;
static unsigned maps, unmaps;

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
    return va;
}

void map_kernel_frame(uintptr_t va, uintptr_t phys, int writable)
{
    (void)va;
    (void)phys;
    (void)writable;
    maps++;
}

void unmap_kernel_frame(uintptr_t va)
{
    (void)va;
    unmaps++;
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

#define REGION_PAGES  (KVA_HEAP_SIZE / PAGE_SIZE)
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

int main(void)
{
    unsigned n, i, k;
    void *runs[KHEAP_RUNS + 1];

    /* The reserve is closed to single pages from the start: the region
       gives every page below it and none of it. */
    kheap_init();
    n = take_singles();
    CHECK(n == REGION_PAGES - RESERVE_PAGES,
          "single pages fill the region up to the reserve and no further");
    give_singles(n);
    CHECK(frames_out == 0, "and every frame went back");

    /* The free pages scattered as a cache leaves them, one in two: no run
       of two remains below the reserve, and a run of 256 pages comes from
       the reserve, sixteen times, the seventeenth refused; one given back
       lets another come. */
    kheap_init();
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
    kheap_init();
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
    kheap_init();
    n = take_singles();
    klogs = 0;
    CHECK(kalloc_pages(1, 0) == 0 && klogs == 1, "a refusal is logged once");
    CHECK(kalloc_pages_try(1, 0) == 0 && klogs == 1, "and not by the quiet form");
    give_singles(n);

    /* With room below the reserve, a large run is taken there and the
       reserve stays whole. */
    kheap_init();
    runs[0] = kalloc_pages(KHEAP_RUN_PAGES, 0);
    CHECK(runs[0] != 0 && (uintptr_t)runs[0] < RESERVE_BASE,
          "a run with room below the reserve is taken below it");
    CHECK(run_bits == 0, "and the reserve stays whole");
    kfree_pages(runs[0], KHEAP_RUN_PAGES);

    /* A shortage of frames inside a reserve run gives the run back: the
       allocation is refused, and once frames are there again the same run
       serves. */
    kheap_init();
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

    if (fails == 0) {
        printf("kheap_hosttest: PASS\n");
    }
    return fails > 255 ? 255 : fails;
}
