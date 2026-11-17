/*
 * kheap.c: the kernel heap, as kheap.h describes it.
 */

#define KHEAP_INSIDE
#include "../lib/spinlock.h"
#include "../arch/arch.h"
#include "kheap.h"

#include "../klog.h"
#include "../panic.h"
#include "../lib/kirq.h"
#include "../lib/kstring.h"
#include "paging.h"
#include "pressure.h"
#include "../proc/smp.h"
#include "../timer.h"

#define KHEAP_MAGIC     0x4B484550u /* "KHEP" */
#define KHEAP_CLASSES   8
#define KHEAP_LARGE     0xFFu
#define KHEAP_HDR       16u
#define KHEAP_CLASS_HDR 32u

/* The header at the base of every page a class's blocks are carved from and
 * of the first page of a large object; pages from `kalloc_pages` have none. */
struct page_hdr {
    uint32_t magic;
    uint32_t cls;  /* the class index, or KHEAP_LARGE */
    uint32_t used; /* blocks in use for a class page; the bytes asked for a large one */
};

struct large_page {
    struct page_hdr hdr;
    uint32_t pages; /* the run's length */
};

struct class_page {
    struct page_hdr hdr;
    uint32_t free;                   /* offset of the first free block, or 0 when full */
    struct class_page *next, **link; /* the class's list of pages with room */
};

typedef char large_header_fits[sizeof(struct large_page) <= KHEAP_HDR ? 1 : -1];
typedef char class_header_fits[sizeof(struct class_page) <= KHEAP_CLASS_HDR ? 1 : -1];

static const uint32_t class_size[KHEAP_CLASSES] = {16, 32, 64, 128, 256, 512, 1024, 2048};

/* The reserve of runs for large allocations, at the top of the region:
 * KHEAP_RUNS runs of KHEAP_RUN_PAGES pages that the ordinary search never
 * enters, so that a region whose free pages the cache has scattered can
 * still give a run of a megabyte, the size of a raw batch and of a request
 * of the largest size. The reserve costs no frame until a run is taken,
 * since a page is mapped only when it is allocated; a run is taken only for
 * an allocation of KHEAP_RUN_LEAST pages or more that the rest of the
 * region cannot give, so a small allocation never uses a run, and it is
 * given back when its pages are freed, by `kfree_pages` or by `kfree` of a
 * large object, both of which go through `release_run`. */
#define KHEAP_RUN_PAGES 256u
#define KHEAP_RUNS      16u
#define KHEAP_RUN_LEAST 64u
/* The heap's lock: the region's pages, its runs, the free lists and the
 * counts, against an allocation or a free on any processor and in any
 * interrupt. It is taken with interrupts off, after the big lock and
 * before the frame allocator's, which taking a page's frame takes. */
static struct spinlock heap_lock = SPINLOCK_INIT_NAMED("heap_lock");
static uint32_t heap_pages;    /* the region's pages, from kheap_init */
static uint32_t reserve_first; /* the reserve's first page */
static uint32_t run_bits;      /* set where a reserve run is taken */
static struct class_page *class_room[KHEAP_CLASSES];
/* The map of the region's pages, set where a page is mapped: `floor_bits`
 * in the image for a region of KHEAP_FLOOR, the region's own first pages
 * for a larger one. */
static uint32_t floor_bits[KHEAP_FLOOR / PAGE_SIZE / 32];
static uint32_t *page_bits;
/* The two ends of the ordinary region's search: every page below `low_free`
 * is taken, and so is every page above `high_free` up to the reserve. The
 * buffer cache's pages are taken from the top down and every other
 * allocation from the bottom up (`kalloc_cache_page`). */
static uint32_t low_free;
static uint32_t high_free;
static uint32_t pages_held;
static size_t bytes_used;
static uint32_t class_pages[KHEAP_CLASSES]; /* pages carved into each class */
static uint32_t class_live[KHEAP_CLASSES];  /* blocks of each class in use */
/* A class's pages with no block in use, which stay on its list of pages
 * with room, up to KHEAP_CLASS_SPARE, as SLUB keeps min_partial empty
 * slabs (mm/slub.c, __slab_free): a kmalloc and kfree of the last block of
 * a class neither maps a page nor unmaps one. */
#define KHEAP_CLASS_SPARE 2u
static uint32_t class_spare[KHEAP_CLASSES];

/* Host tests count the free-list entries visited by a class free. */
#ifdef KHEAP_COUNT_CLASS_STEPS
static uint32_t class_steps;
#define CLASS_STEP() ((void)class_steps++)
#else
#define CLASS_STEP() ((void)0)
#endif

/* A freed large object's run of up to KHEAP_KEEP_PAGES pages, kept mapped
 * for the next kmalloc of its length, as Linux serves kmalloc up to
 * KMALLOC_MAX_CACHE_SIZE from caches whose slabs stay (include/linux/slab.h)
 * and pages up to PAGE_ALLOC_COSTLY_ORDER from per-CPU lists
 * (mm/page_alloc.c, pcp_allowed_order). `kept[n - 1]` heads the runs of n
 * pages, at most KHEAP_KEEP_RUNS of them. A kept run's header has magic 0,
 * so a second kfree is refused; its pages stay counted in `pages_held`.
 * Every kept run goes back to the region when an allocation of the heap
 * finds no room or no frame, and when reclaim asks the heap's shrinker for
 * frames, as Linux drains its per-CPU lists when an allocation fails after
 * direct reclaim (mm/page_alloc.c:4029-4037). */
#define KHEAP_KEEP_PAGES 8u
#define KHEAP_KEEP_RUNS  8u
struct kept_run {
    struct large_page large;
    struct kept_run *next;
};
static struct kept_run *kept[KHEAP_KEEP_PAGES];
static uint32_t kept_runs[KHEAP_KEEP_PAGES];

/* The runs given back that are still mapped. Linux keeps a slab's freed
 * objects mapped, and a freed vmalloc
 * area's translations on the other processors and drops those of many
 * areas with one flush, once the pages so freed pass lazy_max_pages
 * (free_unmap_vmap_area and free_vmap_area_noflush, mm/vmalloc.c:2411-2455;
 * __purge_vmap_area_lazy, 2306-2348), where an unmap with a flush of every
 * processor for each run costs a request to each and a wait for its answer,
 * with the heap's lock held. A run given back stays mapped here, its pages
 * taken and its frames held, until an allocation of its length takes it
 * (`lazy_take`), KHEAP_LAZY_RUNS runs or KHEAP_LAZY_PAGES pages wait, an
 * allocation finds no room or no frame, or reclaim asks (`lazy_purge`, from
 * `release_run` and `kept_drain`); a run longer, or in the reserve, goes
 * back at once. A guarded run waits with its guard, still taken, and only
 * a guarded request of its length takes it back, as Linux's fork takes a
 * cached kernel stack before it asks vmalloc for one (alloc_thread_stack_node
 * and free_thread_stack, kernel/fork.c): a stack freed at each reap would
 * otherwise hold its frames here while each fork mapped fresh ones. */
#define KHEAP_LAZY_RUNS  64u
#define KHEAP_LAZY_PAGES 512u
struct lazy_run {
    uint32_t page;
    uint32_t count;
    int guarded; /* the page below is its guard, taken and unmapped */
};
static struct lazy_run lazy[KHEAP_LAZY_RUNS];
static uint32_t lazy_runs;                      /* the entries of `lazy` in use */
static uint32_t lazy_pages;                     /* and their pages */
static uintptr_t lazy_frames[KHEAP_LAZY_PAGES]; /* their frames, during a purge */
static uint32_t kept_shrink(struct mem_shrinker *s, uint32_t want, int may_sleep);
static struct mem_shrinker kept_shrinker = {kept_shrink, NULL};

/* Linux's warn_alloc limits its lines with `nopage_rs`, at the defaults
 * DEFAULT_RATELIMIT_BURST lines in each DEFAULT_RATELIMIT_INTERVAL, 10 in 5
 * seconds (mm/page_alloc.c, include/linux/ratelimit_types.h). A heap that
 * has run out fails thousands of requests a second, and a line for each
 * buries the first and fills the log. */
#define SHORT_LOG_BURST     10u
#define SHORT_LOG_WINDOW_MS 5000u
static struct spinlock short_lock = SPINLOCK_INIT_NAMED("kheap_short");
static uint32_t short_begin, short_printed, short_missed;
static int short_open;

/* The heap's state that a refusal's line gives, as warn_alloc follows its
 * line with show_mem: the region's pages, those taken, the runs waiting to
 * be unmapped (`lazy`), the bytes the objects in use asked for, and the
 * pages carved into each class. The pages taken that no class holds are the
 * large objects', the buffer cache's, the kernel stacks' and the pages
 * `kalloc_pages` gave. */
struct heap_state {
    uint32_t region, held, waiting;
    unsigned long used;
    uint32_t cls[KHEAP_CLASSES];
};
#define HEAP_STATE_FMT                                                                          \
    "; region %u pages, %u taken, %u waiting, %lu bytes in use, class pages %u %u %u %u %u %u " \
    "%u %u"
#define HEAP_STATE_ARGS(st)                                                                 \
    (st).region, (st).held, (st).waiting, (st).used, (st).cls[0], (st).cls[1], (st).cls[2], \
        (st).cls[3], (st).cls[4], (st).cls[5], (st).cls[6], (st).cls[7]

/**
 * Whether a refusal's line may be logged now, and the heap's state for it
 * in `*st` when it may: the first SHORT_LOG_BURST lines of each window of
 * SHORT_LOG_WINDOW_MS, as Linux's ___ratelimit answers (lib/ratelimit.c).
 * The first line of a window is preceded by the count the last one held
 * back. A caller that finds the state taken by another processor is held
 * back uncounted, as ___ratelimit's trylock holds it back. Called without
 * `heap_lock`, which it takes to read the state.
 */
static int short_may_log(struct heap_state *st)
{
    uint32_t now = (uint32_t)tick_msecs((uint32_t)kernel_ticks);
    uint32_t missed = 0, flags, i;
    int ok;

    flags = kirq_save();
    if (!spin_trylock(&short_lock)) {
        kirq_restore(flags);
        return 0;
    }
    if (!short_open || now - short_begin > SHORT_LOG_WINDOW_MS) {
        missed = short_missed;
        short_open = 1;
        short_begin = now;
        short_printed = 0;
        short_missed = 0;
    }
    ok = short_printed < SHORT_LOG_BURST;
    if (ok) {
        short_printed++;
    } else {
        short_missed++;
    }
    spin_unlock(&short_lock);
    kirq_restore(flags);
    if (missed != 0) {
        klog(KLOG_ERR, "kheap", "%u lines of refusals suppressed", (unsigned int)missed);
    }
    if (!ok) {
        return 0;
    }
    flags = spin_lock_irqsave(&heap_lock);
    st->region = heap_pages;
    st->held = pages_held;
    st->waiting = lazy_pages;
    st->used = (unsigned long)bytes_used;
    for (i = 0; i < KHEAP_CLASSES; i++) {
        st->cls[i] = class_pages[i];
    }
    spin_unlock_irqrestore(&heap_lock, flags);
    return 1;
}

/** Whether page `page` of the region is taken in the map. */
static int page_taken(uint32_t page)
{
    return (page_bits[page / 32] >> (page % 32)) & 1u;
}

/** Mark page `page` of the region taken or free in the map. */
static void page_set(uint32_t page, int taken)
{
    if (taken) {
        page_bits[page / 32] |= 1u << (page % 32);
    } else {
        page_bits[page / 32] &= ~(1u << (page % 32));
    }
}

/** The end of the region, one past its last byte. */
static uintptr_t heap_end(void)
{
    return KVA_HEAP + (uintptr_t)heap_pages * PAGE_SIZE;
}

/** `count` free pages in a row, the lowest such run, a word of the map at
 * a time where all of its pages are taken; `heap_pages` when the region has
 * no such run. The first free page the search passes becomes `low_free`.
 * `heap_lock` is held. */
static uint32_t find_run(uint32_t count)
{
    uint32_t run = 0, i, first = heap_pages;

    for (i = low_free; i < heap_pages; i++) {
        if (i % 32u == 0 && page_bits[i / 32] == 0xFFFFFFFFu) {
            run = 0;
            i += 31;
        } else if (page_taken(i)) {
            run = 0;
        } else {
            if (first == heap_pages) {
                first = i;
                low_free = i;
            }
            if (++run == count) {
                return i + 1 - count;
            }
        }
    }
    return heap_pages;
}

/** The highest free page below the reserve, which becomes `high_free`, or
 * `heap_pages` when there is none. `heap_lock` is held. */
static uint32_t find_page_high(void)
{
    uint32_t i = high_free + 1u;

    while (i-- > 0) {
        if (i % 32u == 31u && page_bits[i / 32] == 0xFFFFFFFFu) {
            i -= 31;
        } else if (!page_taken(i)) {
            high_free = i;
            return i;
        }
    }
    return heap_pages;
}

/** Map `count` fresh frames at the run starting at `page` and mark them
 * taken: 0, or -1 when the frames ran out, with what was mapped unmapped
 * again. `heap_lock` is held. */
static int map_run(uint32_t page, uint32_t count)
{
    uint32_t i;

    for (i = 0; i < count; i++) {
        uintptr_t phys = alloc_frame();

        if (phys == 0) {
            while (i-- > 0) {
                uintptr_t va = KVA_HEAP + ((uintptr_t)page + i) * PAGE_SIZE;

                phys = paging_virt_to_phys(va);
                unmap_kernel_frame(va);
                free_frame(phys);
                page_set(page + i, 0);
            }
            return -1;
        }
        map_kernel_frame(KVA_HEAP + ((uintptr_t)page + i) * PAGE_SIZE, phys, 1);
        page_set(page + i, 1);
    }
    pages_held += count;
    return 0;
}

/* The pages `unmap_run` unmaps with one flush of every processor. */
#define KHEAP_UNMAP_BATCH 16u

/**
 * Unmap the run of `count` pages at `page`, give back its frames and mark
 * it free. `heap_lock` is held. The frames go back only once every
 * processor has dropped its translations of the pages, a batch at a time
 * (`unmap_kernel_frames`): a processor reads the heap without the big
 * lock, and a translation it kept would reach a frame given to another
 * user. The flush is made with `heap_lock` held, which a processor that
 * spins for it answers, so that no page is taken again before it is done.
 */
static void unmap_run(uint32_t page, uint32_t count)
{
    uintptr_t phys[KHEAP_UNMAP_BATCH];
    uint32_t done, i, n;

    for (done = 0; done < count; done += n) {
        uintptr_t va = KVA_HEAP + ((uintptr_t)page + done) * PAGE_SIZE;

        n = count - done < KHEAP_UNMAP_BATCH ? count - done : KHEAP_UNMAP_BATCH;
        for (i = 0; i < n; i++) {
            phys[i] = paging_virt_to_phys(va + (uintptr_t)i * PAGE_SIZE);
        }
        unmap_kernel_frames(va, n);
        for (i = 0; i < n; i++) {
            free_frame(phys[i]);
            page_set(page + done + i, 0);
        }
    }
    pages_held -= count;
}

size_t kheap_size_for(uint64_t managed, uint64_t most)
{
    uint64_t bytes = managed / 8u / KHEAP_GRAIN * KHEAP_GRAIN;

    if (bytes < KHEAP_FLOOR) {
        bytes = KHEAP_FLOOR;
    }
    if (bytes > most) {
        bytes = most / KHEAP_GRAIN * KHEAP_GRAIN;
    }
    return (size_t)bytes;
}

/** The reserve's pages are marked taken from the start, so that the
 * ordinary search passes them; `run_bits` says which runs are out. The kept
 * runs' shrinker is registered once, however often the host tests start
 * the heap again. */
void kheap_init(size_t bytes)
{
    uint32_t i, map_pages = 0;

    heap_pages = (uint32_t)(bytes / PAGE_SIZE);
    reserve_first = heap_pages - KHEAP_RUNS * KHEAP_RUN_PAGES;
    if (premap_kernel_window(KVA_HEAP, bytes) != 0) {
        k_panic("kheap: no memory for the region's page tables");
    }
    page_bits = floor_bits;
    if (heap_pages > KHEAP_FLOOR / PAGE_SIZE) {
        map_pages = (heap_pages / 8u + PAGE_SIZE - 1) / PAGE_SIZE;
        page_bits = (uint32_t *)KVA_HEAP;
        for (i = 0; i < map_pages; i++) {
            uintptr_t phys = alloc_frame();

            if (phys == 0) {
                k_panic("kheap: no frame for the map of the region's pages");
            }
            map_kernel_frame(KVA_HEAP + (uintptr_t)i * PAGE_SIZE, phys, 1);
        }
    }
    k_memset(page_bits, 0, heap_pages / 8u);
    k_memset(class_room, 0, sizeof(class_room));
    for (i = 0; i < map_pages; i++) {
        page_set(i, 1);
    }
    low_free = map_pages;
    high_free = reserve_first - 1u;
    pages_held = map_pages;
    bytes_used = 0;
    for (i = reserve_first; i < heap_pages; i++) {
        page_set(i, 1);
    }
    run_bits = 0;
    k_memset(kept, 0, sizeof(kept));
    k_memset(kept_runs, 0, sizeof(kept_runs));
    lazy_runs = 0;
    lazy_pages = 0;
    k_memset(class_pages, 0, sizeof(class_pages));
    k_memset(class_live, 0, sizeof(class_live));
    k_memset(class_spare, 0, sizeof(class_spare));
    mem_shrinker_unregister(&kept_shrinker);
    mem_shrinker_register(&kept_shrinker);
}

uint32_t kheap_region_pages(void)
{
    return heap_pages;
}

/** The first page of a free reserve run, marked out, or `heap_pages` when
 * every run is out or `count` is more than a run. `heap_lock` is held. */
static uint32_t reserve_take(uint32_t count)
{
    uint32_t r;

    if (count > KHEAP_RUN_PAGES) {
        return heap_pages;
    }
    for (r = 0; r < KHEAP_RUNS; r++) {
        if (!(run_bits & (1u << r))) {
            run_bits |= 1u << r;
            return reserve_first + r * KHEAP_RUN_PAGES;
        }
    }
    return heap_pages;
}

/** A reserve run whose pages `map_run` or `unmap_run` left clear is closed
 * to the ordinary search again and, when `release` says so, given back to
 * the reserve. `heap_lock` is held. */
static void reserve_close(uint32_t page, uint32_t count, int release)
{
    uint32_t i;

    for (i = 0; i < count; i++) {
        page_set(page + i, 1);
    }
    if (release) {
        run_bits &= ~(1u << ((page - reserve_first) / KHEAP_RUN_PAGES));
    }
}

/** Pages `first` through `last`, below the reserve, are free again: the
 * searches from both ends start at them if they lie beyond where each would
 * start. `heap_lock` is held. */
static void ends_widen(uint32_t first, uint32_t last)
{
    if (first < low_free) {
        low_free = first;
    }
    if (last > high_free) {
        high_free = last;
    }
}

/** The run of `count` pages at `page`, unmapped everywhere with its frames
 * back and its pages marked free, joins the searches, and when it lies in
 * the reserve the run is given back. `heap_lock` is held. */
static void run_free(uint32_t page, uint32_t count)
{
    if (page < reserve_first) {
        ends_widen(page, page + count - 1u);
    }
    if (page >= reserve_first) {
        reserve_close(page, count, 1);
    }
}

/**
 * Unmap every run waiting in `lazy` with one request to each other
 * processor, then give back their frames and their pages: the pages given
 * back, 0 when none waited. The frames are read before the entries are
 * cleared, and go back only after the flush, as `unmap_run` requires.
 * `heap_lock` is held.
 */
static uint32_t lazy_purge(void)
{
    uint32_t r, i, n = 0, lo = heap_pages, hi = 0, pages = lazy_pages;

    if (lazy_runs == 0) {
        return 0;
    }
    for (r = 0; r < lazy_runs; r++) {
        uintptr_t va = KVA_HEAP + (uintptr_t)lazy[r].page * PAGE_SIZE;

        for (i = 0; i < lazy[r].count; i++) {
            lazy_frames[n++] = paging_virt_to_phys(va + (uintptr_t)i * PAGE_SIZE);
        }
        (void)unmap_kernel_frames_here(va, lazy[r].count);
        lo = lazy[r].page < lo ? lazy[r].page : lo;
        hi = lazy[r].page + lazy[r].count > hi ? lazy[r].page + lazy[r].count : hi;
    }
    smp_kernel_flush(KVA_HEAP + (uintptr_t)lo * PAGE_SIZE, KVA_HEAP + (uintptr_t)hi * PAGE_SIZE);
    n = 0;
    for (r = 0; r < lazy_runs; r++) {
        for (i = 0; i < lazy[r].count; i++) {
            if (lazy_frames[n]) {
                free_frame(lazy_frames[n]);
            }
            n++;
            page_set(lazy[r].page + i, 0);
        }
        pages_held -= lazy[r].count;
        run_free(lazy[r].page, lazy[r].count);
        if (lazy[r].guarded) {
            page_set(lazy[r].page - 1u, 0);
            ends_widen(lazy[r].page - 1u, lazy[r].page - 1u);
        }
    }
    lazy_runs = 0;
    lazy_pages = 0;
    return pages;
}

/**
 * A run of `count` pages waiting in `lazy`, taken out of it for an
 * allocation of that length, which finds it mapped and its frames there,
 * as a slab's object comes back with no new page: the one given back last
 * of those on the allocation's side of `high_free`. The buffer cache's
 * pages are taken down from the region's top, and every page from
 * `high_free` up is taken, so a page there is the cache's (`high`,
 * `kalloc_cache_page`) and one below it the rest's, and neither takes the
 * other's. A `guarded` request takes only a guarded run, which comes with
 * its guard, and any other request only a run with none. `heap_pages` when
 * there is none. `heap_lock` is held.
 */
static uint32_t lazy_take(uint32_t count, int high, int guarded)
{
    uint32_t r, page;

    for (r = lazy_runs; r-- > 0;) {
        if (lazy[r].count == count && lazy[r].guarded == guarded &&
            (guarded || (lazy[r].page >= high_free) == (high != 0))) {
            break;
        }
    }
    if (r >= lazy_runs) {
        return heap_pages;
    }
    page = lazy[r].page;
    lazy[r] = lazy[--lazy_runs];
    lazy_pages -= count;
    return page;
}

/** Give a run back, the one way pages leave the heap, whoever allocated
 * them: into `lazy`, after a purge when it has no room, or at once for a
 * run longer than KHEAP_LAZY_PAGES or in the reserve, whose runs are few
 * and large. A `guarded` run's guard waits with it and comes free with its
 * pages. `heap_lock` is held. */
static void release_run(uint32_t page, uint32_t count, int guarded)
{
    if (count > KHEAP_LAZY_PAGES || page >= reserve_first) {
        unmap_run(page, count);
        run_free(page, count);
        if (guarded) {
            page_set(page - 1u, 0);
            ends_widen(page - 1u, page - 1u);
        }
        return;
    }
    if (lazy_runs == KHEAP_LAZY_RUNS || lazy_pages + count > KHEAP_LAZY_PAGES) {
        (void)lazy_purge();
    }
    lazy[lazy_runs].page = page;
    lazy[lazy_runs].count = count;
    lazy[lazy_runs].guarded = guarded;
    lazy_runs++;
    lazy_pages += count;
}

/** Give every kept run back to the region, and every run waiting to be
 * unmapped (`lazy_purge`): the pages given back, 0 when there was none.
 * `heap_lock` is held. */
static uint32_t kept_drain(void)
{
    uint32_t i;

    for (i = 0; i < KHEAP_KEEP_PAGES; i++) {
        while (kept[i]) {
            struct kept_run *k = kept[i];

            kept[i] = k->next;
            release_run((uint32_t)(((uintptr_t)k - KVA_HEAP) / PAGE_SIZE), i + 1u, 0);
        }
        kept_runs[i] = 0;
    }
    return lazy_purge();
}

/** Reclaim's call of the heap (`struct mem_shrinker`): every kept run given
 * back, whatever `want` is, since they are few; the frames that came back.
 * It does not sleep. */
static uint32_t kept_shrink(struct mem_shrinker *s, uint32_t want, int may_sleep)
{
    uint32_t flags = spin_lock_irqsave(&heap_lock);
    uint32_t pages = kept_drain();

    (void)s;
    (void)want;
    (void)may_sleep;
    spin_unlock_irqrestore(&heap_lock, flags);
    return pages;
}

/** `count` pages, mapped, as `take_pages` gives them, without draining the
 * kept runs. `heap_lock` is held. */
static uint32_t take_pages_once(uint32_t count)
{
    uint32_t page = lazy_take(count, 0, 0);

    if (page < heap_pages) {
        return page;
    }
    page = find_run(count);

    if (page >= heap_pages && count >= KHEAP_RUN_LEAST) {
        page = reserve_take(count);
        if (page < heap_pages && map_run(page, count) != 0) {
            reserve_close(page, count, 1);
            page = heap_pages;
        }
    } else if (page < heap_pages && map_run(page, count) != 0) {
        page = heap_pages;
    }
    return page;
}

/** `count` pages, mapped, from the ordinary region or, for a run of
 * KHEAP_RUN_LEAST or more that it cannot give, from the reserve, the kept
 * runs given back and the search made again when neither has them;
 * `heap_pages` when they still do not or the frames ran out. `heap_lock` is
 * held. */
static uint32_t take_pages(uint32_t count)
{
    uint32_t page = take_pages_once(count);

    if (page >= heap_pages && kept_drain()) {
        page = take_pages_once(count);
    }
    return page;
}

/** `count` pages as kalloc_pages gives them; `quiet` leaves a refusal out
 * of the log, for a caller that has a smaller size or a static buffer to go
 * on with. */
static void *pages_or_null(uint32_t count, uintptr_t *phys, int quiet)
{
    struct heap_state st;
    uint32_t flags = spin_lock_irqsave(&heap_lock);
    uint32_t page;
    void *va;

    if (count == 0 || count > heap_pages) {
        spin_unlock_irqrestore(&heap_lock, flags);
        return NULL;
    }
    page = take_pages(count);
    if (page >= heap_pages) {
        spin_unlock_irqrestore(&heap_lock, flags);
        if (!quiet && short_may_log(&st)) {
            klog(KLOG_ERR, "kheap", "%d pages: no room or no frame" HEAP_STATE_FMT, (int)count,
                 HEAP_STATE_ARGS(st));
        }
        return NULL;
    }
    va = (void *)(KVA_HEAP + (uintptr_t)page * PAGE_SIZE);
    if (phys) {
        *phys = paging_virt_to_phys((uintptr_t)va);
    }
    spin_unlock_irqrestore(&heap_lock, flags);
    return va;
}

void *kalloc_pages(uint32_t count, uintptr_t *phys)
{
    return pages_or_null(count, phys, 0);
}

void *kalloc_pages_try(uint32_t count, uintptr_t *phys)
{
    return pages_or_null(count, phys, 1);
}

/** Map a run of `count` consecutive frames (`alloc_frames`) at the run of
 * free pages starting at `page` and mark them taken: 0, or -1 when no such
 * run of frames is free. `heap_lock` is held. */
static int map_run_contig(uint32_t page, uint32_t count)
{
    uintptr_t phys = alloc_frames(count);
    uint32_t i;

    if (phys == 0) {
        return -1;
    }
    for (i = 0; i < count; i++) {
        map_kernel_frame(KVA_HEAP + ((uintptr_t)page + i) * PAGE_SIZE,
                         phys + (uintptr_t)i * PAGE_SIZE, 1);
        page_set(page + i, 1);
    }
    pages_held += count;
    return 0;
}

/** `count` pages backed by consecutive frames, from the ordinary region,
 * or `heap_pages` when it has no room or no such frames. `heap_lock` is
 * held. */
static uint32_t take_contig_once(uint32_t count)
{
    uint32_t page = find_run(count);

    if (page < heap_pages && map_run_contig(page, count) != 0) {
        page = heap_pages;
    }
    return page;
}

/** The kept runs are given back and the run sought again when the first
 * search finds no room or no frames, as `take_pages` does. */
void *kalloc_contig(uint32_t count, uintptr_t *phys)
{
    struct heap_state st;
    uint32_t flags = spin_lock_irqsave(&heap_lock);
    uint32_t page;
    void *va;

    if (count == 0 || count >= KHEAP_RUN_LEAST) {
        spin_unlock_irqrestore(&heap_lock, flags);
        klog(KLOG_ERR, "kheap", "%d consecutive frames: not a size it gives", (int)count);
        return NULL;
    }
    page = take_contig_once(count);
    if (page >= heap_pages && kept_drain()) {
        page = take_contig_once(count);
    }
    if (page >= heap_pages) {
        spin_unlock_irqrestore(&heap_lock, flags);
        if (short_may_log(&st)) {
            klog(KLOG_ERR, "kheap", "%d consecutive frames: no room or no run free" HEAP_STATE_FMT,
                 (int)count, HEAP_STATE_ARGS(st));
        }
        return NULL;
    }
    va = (void *)(KVA_HEAP + (uintptr_t)page * PAGE_SIZE);
    if (phys) {
        *phys = paging_virt_to_phys((uintptr_t)va);
    }
    spin_unlock_irqrestore(&heap_lock, flags);
    return va;
}

/** A page for the buffer cache, mapped, from the top of the ordinary
 * region; `heap_pages` when there is no free page or no frame for it.
 * `heap_lock` is held. */
static uint32_t cache_take(void)
{
    uint32_t page = lazy_take(1, 1, 0);

    if (page < heap_pages) {
        return page;
    }
    page = find_page_high();

    if (page < heap_pages && map_run(page, 1) != 0) {
        page = heap_pages;
    }
    return page;
}

/** The kept runs are given back and the page sought again when the region
 * has no free page or no frame for it. */
void *kalloc_cache_page(void)
{
    struct heap_state st;
    uint32_t flags = spin_lock_irqsave(&heap_lock);
    uint32_t page = cache_take();

    if (page >= heap_pages && kept_drain()) {
        page = cache_take();
    }
    if (page >= heap_pages) {
        spin_unlock_irqrestore(&heap_lock, flags);
        if (short_may_log(&st)) {
            klog(KLOG_ERR, "kheap", "a cache page: no room or no frame" HEAP_STATE_FMT,
                 HEAP_STATE_ARGS(st));
        }
        return NULL;
    }
    spin_unlock_irqrestore(&heap_lock, flags);
    return (void *)(KVA_HEAP + (uintptr_t)page * PAGE_SIZE);
}

void kfree_pages(void *p, uint32_t count)
{
    uintptr_t va = (uintptr_t)p;
    uint32_t flags;

    if (!p || va < KVA_HEAP || va >= heap_end() || (va & (PAGE_SIZE - 1))) {
        klog(KLOG_ERR, "kheap", "free of pages the heap did not give: %p", p);
        return;
    }
    flags = spin_lock_irqsave(&heap_lock);
    release_run((uint32_t)((va - KVA_HEAP) / PAGE_SIZE), count, 0);
    spin_unlock_irqrestore(&heap_lock, flags);
}

/** The guard of a run of `count` pages and a guard below them, with the
 * pages mapped and the guard not yet marked taken, or a guarded run waiting
 * in `lazy`, whose guard is still marked; `heap_pages` when there is no such
 * run or no frames for it. `heap_lock` is held. */
static uint32_t guarded_take(uint32_t count)
{
    uint32_t guard = heap_pages;

    if (count > 0 && count < KHEAP_RUN_LEAST) {
        guard = lazy_take(count, 0, 1);
        if (guard < heap_pages) {
            return guard - 1u;
        }
        guard = find_run(count + 1);
    }
    if (guard < heap_pages && map_run(guard + 1, count) != 0) {
        guard = heap_pages;
    }
    return guard;
}

/** The guard is the first page of a run of `count + 1` and is marked taken
 * only once the pages above it are mapped, so a shortage of frames leaves
 * nothing behind. The kept runs are given back and the run sought again
 * when the region has no such run or no frames for it. */
void *kalloc_guarded(uint32_t count)
{
    struct heap_state st;
    uint32_t flags = spin_lock_irqsave(&heap_lock);
    uint32_t guard = guarded_take(count);

    if (guard >= heap_pages && kept_drain()) {
        guard = guarded_take(count);
    }
    if (guard >= heap_pages) {
        spin_unlock_irqrestore(&heap_lock, flags);
        if (short_may_log(&st)) {
            klog(KLOG_ERR, "kheap", "%d pages and a guard: no room or no frame" HEAP_STATE_FMT,
                 (int)count, HEAP_STATE_ARGS(st));
        }
        return NULL;
    }
    page_set(guard, 1);
    spin_unlock_irqrestore(&heap_lock, flags);
    return (void *)(KVA_HEAP + ((uintptr_t)guard + 1) * PAGE_SIZE);
}

/** Whether the run at `page` waits in `lazy`. `heap_lock` is held. */
static int lazy_holds(uint32_t page)
{
    uint32_t r;

    for (r = 0; r < lazy_runs; r++) {
        if (lazy[r].page == page) {
            return 1;
        }
    }
    return 0;
}

/** A guard is a page below the reserve that is taken and has no mapping.
 * Below the first page of any other run is a mapped page, a free page or
 * the region's start: every taken page outside the reserve is mapped
 * except a guard, and the page above a guard begins the guard's own run.
 * A run that waits in `lazy` keeps its guard, so a second free of it is
 * found there and refused. */
void kfree_guarded(void *p, uint32_t count)
{
    uintptr_t va = (uintptr_t)p;
    uint32_t flags, page;

    if (!p || va <= KVA_HEAP || va >= heap_end() || (va & (PAGE_SIZE - 1))) {
        klog(KLOG_ERR, "kheap", "free of pages the heap did not give: %p", p);
        return;
    }
    page = (uint32_t)((va - KVA_HEAP) / PAGE_SIZE);
    flags = spin_lock_irqsave(&heap_lock);
    if (count == 0 || count >= reserve_first || page > reserve_first - count ||
        !page_taken(page - 1) || paging_virt_to_phys(va - PAGE_SIZE) != 0) {
        spin_unlock_irqrestore(&heap_lock, flags);
        klog(KLOG_ERR, "kheap", "free of pages with no guard below them: %p", p);
        return;
    }
    if (lazy_holds(page)) {
        spin_unlock_irqrestore(&heap_lock, flags);
        klog(KLOG_ERR, "kheap", "second free of guarded pages: %p", p);
        return;
    }
    release_run(page, count, 1);
    spin_unlock_irqrestore(&heap_lock, flags);
}

/** Put a page at the head of its class's list. `heap_lock` is held. */
static void class_link(struct class_page *page, uint32_t cls)
{
    CLASS_STEP();
    page->link = &class_room[cls];
    page->next = *page->link;
    if (page->next) {
        CLASS_STEP();
        page->next->link = &page->next;
    }
    *page->link = page;
}

/** Unlink a page from its class's pages with room. `heap_lock` is held. */
static void class_unlink(struct class_page *page)
{
    CLASS_STEP();
    *page->link = page->next;
    if (page->next) {
        CLASS_STEP();
        page->next->link = page->link;
    }
}

/** Carve a page into blocks of class `cls`, linking its blocks on the page
 * and the page on the class: 0, or -1 for no page. `heap_lock` is held. */
static int add_class_page(uint32_t cls)
{
    uint32_t index = take_pages(1);
    struct class_page *page;
    uintptr_t base;
    uint32_t size = class_size[cls];
    uint32_t off;

    if (index >= heap_pages) {
        return -1;
    }
    base = KVA_HEAP + (uintptr_t)index * PAGE_SIZE;
    page = (struct class_page *)base;
    page->hdr.magic = KHEAP_MAGIC;
    page->hdr.cls = cls;
    page->hdr.used = 0;
    page->free = 0;
    for (off = KHEAP_CLASS_HDR; off + size <= PAGE_SIZE; off += size) {
        *(uint32_t *)(base + off) = page->free;
        page->free = off;
    }
    class_link(page, cls);
    class_pages[cls]++;
    class_spare[cls]++;
    return 0;
}

/** A size above the largest class takes whole pages with the header in
 * the first, and the header's `used` has 32 bits for the size. */
void *kmalloc(size_t size)
{
    struct heap_state st;
    uint32_t flags, cls;
    struct class_page *page;
    void *block;

    if (size == 0 || size > (size_t)heap_pages * PAGE_SIZE - PAGE_SIZE || size > 0xFFFFFFF0u) {
        return NULL;
    }
    if (size > class_size[KHEAP_CLASSES - 1]) {
        uint32_t count = (uint32_t)((size + KHEAP_HDR + PAGE_SIZE - 1) / PAGE_SIZE);
        void *base = NULL;
        struct large_page *large;

        if (count <= KHEAP_KEEP_PAGES) {
            flags = spin_lock_irqsave(&heap_lock);
            if (kept[count - 1]) {
                base = kept[count - 1];
                kept[count - 1] = kept[count - 1]->next;
                kept_runs[count - 1]--;
            }
            spin_unlock_irqrestore(&heap_lock, flags);
        }
        if (!base) {
            base = kalloc_pages(count, NULL);
        }
        if (!base) {
            return NULL;
        }
        large = base;
        large->hdr.magic = KHEAP_MAGIC;
        large->hdr.cls = KHEAP_LARGE;
        large->hdr.used = (uint32_t)size;
        large->pages = count;
        flags = spin_lock_irqsave(&heap_lock);
        bytes_used += size;
        spin_unlock_irqrestore(&heap_lock, flags);
        return (uint8_t *)base + KHEAP_HDR;
    }
    for (cls = 0; class_size[cls] < size; cls++) {
    }
    flags = spin_lock_irqsave(&heap_lock);
    if (!class_room[cls] && add_class_page(cls) != 0) {
        spin_unlock_irqrestore(&heap_lock, flags);
        if (short_may_log(&st)) {
            klog(KLOG_ERR, "kheap", "%d bytes: no room or no frame" HEAP_STATE_FMT, (int)size,
                 HEAP_STATE_ARGS(st));
        }
        return NULL;
    }
    page = class_room[cls];
    block = (uint8_t *)page + page->free;
    page->free = *(uint32_t *)block;
    if (page->free == 0) {
        class_unlink(page);
    }
    if (page->hdr.used++ == 0) {
        class_spare[cls]--;
    }
    bytes_used += class_size[cls];
    class_live[cls]++;
    spin_unlock_irqrestore(&heap_lock, flags);
    return block;
}

void *kzalloc(size_t size)
{
    void *p = kmalloc(size);

    if (p) {
        k_memset(p, 0, size);
    }
    return p;
}

void kfree(void *p)
{
    uintptr_t va = (uintptr_t)p;
    struct page_hdr *hdr;
    struct class_page *page;
    uint32_t flags, cls;

    if (!p) {
        return;
    }
    if (va < KVA_HEAP || va >= heap_end()) {
        klog(KLOG_ERR, "kheap", "free of memory the heap did not give: %p", p);
        return;
    }
    hdr = (struct page_hdr *)(va & ~(PAGE_SIZE - 1));
    flags = spin_lock_irqsave(&heap_lock);
    if (hdr->magic != KHEAP_MAGIC) {
        spin_unlock_irqrestore(&heap_lock, flags);
        klog(KLOG_ERR, "kheap", "free of a page without the header: %p", p);
        return;
    }
    if (hdr->cls == KHEAP_LARGE) {
        uint32_t count = ((struct large_page *)hdr)->pages;

        bytes_used -= hdr->used;
        hdr->magic = 0;
        if (count <= KHEAP_KEEP_PAGES && kept_runs[count - 1] < KHEAP_KEEP_RUNS) {
            struct kept_run *k = (void *)hdr;

            k->next = kept[count - 1];
            kept[count - 1] = k;
            kept_runs[count - 1]++;
        } else {
            release_run((uint32_t)(((uintptr_t)hdr - KVA_HEAP) / PAGE_SIZE), count, 0);
        }
        spin_unlock_irqrestore(&heap_lock, flags);
        return;
    }
    cls = hdr->cls;
    if (cls >= KHEAP_CLASSES || hdr->used == 0) {
        spin_unlock_irqrestore(&heap_lock, flags);
        klog(KLOG_ERR, "kheap", "free of a block not in use: %p", p);
        return;
    }
    page = (void *)hdr;
    if (page->free == 0) {
        class_link(page, cls);
    }
    CLASS_STEP();
    *(uint32_t *)p = page->free;
    page->free = (uint32_t)(va - (uintptr_t)page);
    bytes_used -= class_size[cls];
    class_live[cls]--;
    if (--hdr->used == 0) {
        if (class_spare[cls] < KHEAP_CLASS_SPARE) {
            class_spare[cls]++;
        } else {
            class_unlink(page);
            class_pages[cls]--;
            hdr->magic = 0;
            release_run((uint32_t)(((uintptr_t)hdr - KVA_HEAP) / PAGE_SIZE), 1, 0);
        }
    }
    spin_unlock_irqrestore(&heap_lock, flags);
}

void kheap_stats(uint32_t *pages, size_t *bytes)
{
    if (pages) {
        *pages = pages_held;
    }
    if (bytes) {
        *bytes = bytes_used;
    }
}

uint32_t kheap_reclaimable(void)
{
    uint32_t flags = spin_lock_irqsave(&heap_lock);
    uint32_t i, pages = lazy_pages;

    for (i = 0; i < KHEAP_KEEP_PAGES; i++) {
        pages += kept_runs[i] * (i + 1u);
    }
    spin_unlock_irqrestore(&heap_lock, flags);
    return pages;
}

int kheap_class(uint32_t cls, struct kheap_class *out)
{
    uint32_t flags;

    if (cls >= KHEAP_CLASSES) {
        return -1;
    }
    out->size = class_size[cls];
    out->per_page = (PAGE_SIZE - KHEAP_CLASS_HDR) / class_size[cls];
    flags = spin_lock_irqsave(&heap_lock);
    out->live = class_live[cls];
    out->pages = class_pages[cls];
    spin_unlock_irqrestore(&heap_lock, flags);
    return 0;
}
