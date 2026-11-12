/**
 * Host test of the address-space change notice (mm/notify.c) and of the
 * shrinkers reclaim asks (mm/pressure.c), with i686's `cpu.h`;
 * mmnotify64_hosttest.c runs the same rows with x86-64's.
 *
 * host-deps: rach2/sys/mm/vm.c rach2/sys/mm/frame.c rach2/sys/mm/notify.c
 * host-deps: rach2/sys/mm/pressure.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/spinlock_host.c
 * host-deps: tests/host/compat/mmap_lock_host.c
 * host-deps: tests/host/compat/atomic_host.c
 * host-cflags: -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
 * host-cflags: -U__UINTPTR_TYPE__ -D__UINTPTR_TYPE__=unsigned
 * host-cflags: -U__INTPTR_TYPE__ -D__INTPTR_TYPE__=int
 * host-cflags: -U__SIZE_TYPE__ -D__SIZE_TYPE__=unsigned
 * host-cflags: -U__PTRDIFF_TYPE__ -D__PTRDIFF_TYPE__=int
 *
 * mm_model.h includes paging.c whole over a model of the page tables, and
 * pressure.c is the kernel's, over caches that give nothing back. The rows:
 * a registered range is called for each change that overlaps it, with the
 * part they share, its count raised, and not for a change outside it or in
 * another address space; a range may unregister itself in its call, and one
 * unregistered is called no more; the end of the address space unregisters
 * each range and calls it with MMN_RELEASE and its whole range, an empty
 * range among them. paging.c tells a fork's write-protection of a private
 * region (MMN_PROTECT), each copy a write makes of a shared page (MMN_COW)
 * and a device's taking back of its pages (MMN_TRUNCATE). Below the low
 * mark, reclaim asks the shrinkers, the last registered first, for what the
 * caches did not give, until the high mark, and says whether they may
 * sleep; above it no one is asked. Two faults of one page interleaved
 * across a file's fill, which lets the regions' lock go, map it once, and a
 * device's page taken back during the device's call is asked for again.
 * The first fault of an object that asks it maps its whole region, up to
 * the object's end, a batch at a time; a zap during a batch's calls maps
 * nothing of the batch, and a region cut short maps what remains. A zap
 * walk that waits for one space's lock zaps again a space that faulted the
 * object back in while it waited. `--lock-only` runs the rows of faults
 * that let the regions' lock go, and the walk, alone.
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#define MM_MODEL_PRESSURE
#include "mm_model.h"

#define VA (WIN_BASE)
#define VD (WIN_BASE + SPAN)
#define P  ((uintptr_t)PAGE_SIZE)
#define RW (VM_PROT_READ | VM_PROT_WRITE)

#ifdef RACH2_HOST_X86_64
#define NAME "mmnotify64-hosttest"
#else
#define NAME "mmnotify-hosttest"
#endif

/* ---- the caches reclaim asks first: they give nothing ---- */

static int irq_on, in_call;

/** Nothing. */
uint32_t bcache_shrink(uint32_t want, int can_write, uint32_t *written)
{
    (void)want;
    (void)can_write;
    (void)written;
    return 0;
}

/** Nothing. */
uint32_t filecache_shrink(uint32_t want)
{
    (void)want;
    return 0;
}

/** Nothing. */
uint32_t pc_shrink(uint32_t want, int can_write, uint32_t *written)
{
    (void)want;
    (void)can_write;
    (void)written;
    return 0;
}

/** As a row sets. */
int kirq_enabled(void)
{
    return irq_on;
}

/** As a row sets. */
int proc_in_syscall(void)
{
    return in_call;
}

/* ---- the ranges' calls ---- */

#define CALLS 16

/* A call a range had. */
struct call {
    struct mmn_range *r;
    int event;
    uintptr_t start, end;
};

static struct call calls[CALLS];
static uint32_t ncalls;

/** Record the call. */
static void record(struct mmn_range *r, int event, uintptr_t start, uintptr_t end)
{
    if (ncalls < CALLS) {
        calls[ncalls].r = r;
        calls[ncalls].event = event;
        calls[ncalls].start = start;
        calls[ncalls].end = end;
    }
    ncalls++;
}

/** Record the call, and unregister the range. */
static void record_once(struct mmn_range *r, int event, uintptr_t start, uintptr_t end)
{
    record(r, event, start, end);
    mmn_unregister(r);
}

static const struct mmn_ops ops = {record};
static const struct mmn_ops once_ops = {record_once};

/** Whether call `i` was of `r` with `event` over [start, end). */
static int had(uint32_t i, struct mmn_range *r, int event, uintptr_t start, uintptr_t end)
{
    return i < ncalls && i < CALLS && calls[i].r == r && calls[i].event == event &&
           calls[i].start == start && calls[i].end == end;
}

/* ---- the rows ---- */

/** Calls with the part a change and a range share; none outside, none for
 * another space, none after an unregister; a range unregisters itself. */
static void notify_rows(void)
{
    struct mmn_range r1, r2, r3;
    int owner;

    mmn_register(&mm_a, &r1, VA, VA + 4 * P, &ops, &owner);
    mmn_register(&mm_a, &r2, VA + 2 * P, VA + 6 * P, &ops, NULL);
    check(r1.mm == &mm_a && r1.owner == &owner && r1.seq == 0, "a range is registered");
    ncalls = 0;
    mmn_notify(&mm_a, MMN_PROTECT, VA + 3 * P, VA + 5 * P);
    check(ncalls == 2 &&
              (had(0, &r1, MMN_PROTECT, VA + 3 * P, VA + 4 * P) ||
               had(1, &r1, MMN_PROTECT, VA + 3 * P, VA + 4 * P)) &&
              (had(0, &r2, MMN_PROTECT, VA + 3 * P, VA + 5 * P) ||
               had(1, &r2, MMN_PROTECT, VA + 3 * P, VA + 5 * P)) &&
              r1.seq == 1 && r2.seq == 1,
          "a change over both ranges calls each with the part it shares");
    ncalls = 0;
    mmn_notify(&mm_a, MMN_UNMAP, VA + 6 * P, VA + 8 * P);
    mmn_notify(&mm_a, MMN_DROP, VA, VA);
    mmn_notify(&mm_b, MMN_UNMAP, VA, VA + 8 * P);
    check(ncalls == 0 && r2.seq == 1,
          "a change past the ranges, an empty one or one in another space calls none");
    mmn_notify(&mm_a, MMN_DROP, VA + 5 * P, VA + 9 * P);
    check(ncalls == 1 && had(0, &r2, MMN_DROP, VA + 5 * P, VA + 6 * P), "a change over the end");

    mmn_register(&mm_a, &r3, VA, VA + P, &once_ops, NULL);
    ncalls = 0;
    mmn_notify(&mm_a, MMN_UNMAP, VA, VA + 8 * P);
    check(ncalls == 3 && r3.mm == NULL, "a range that unregisters itself in its call ends the walk"
                                        " no sooner");
    ncalls = 0;
    mmn_notify(&mm_a, MMN_UNMAP, VA, VA + 8 * P);
    check(ncalls == 2, "and is called no more");
    mmn_unregister(&r2);
    mmn_unregister(&r2);
    ncalls = 0;
    mmn_notify(&mm_a, MMN_TRUNCATE, VA, VA + 8 * P);
    check(ncalls == 1 && had(0, &r1, MMN_TRUNCATE, VA, VA + 4 * P),
          "an unregistered range, twice over, is called no more");

    mmn_register(&mm_a, &r2, 0, 0, &ops, NULL);
    ncalls = 0;
    mmn_notify(&mm_a, MMN_UNMAP, 0, ~(uintptr_t)0);
    check(ncalls == 1, "an empty range hears no change");
    ncalls = 0;
    mmn_release(&mm_a);
    check(ncalls == 2 && mm_a.notify == NULL && r1.mm == NULL && r2.mm == NULL &&
              (had(0, &r1, MMN_RELEASE, VA, VA + 4 * P) ||
               had(1, &r1, MMN_RELEASE, VA, VA + 4 * P)) &&
              (had(0, &r2, MMN_RELEASE, 0, 0) || had(1, &r2, MMN_RELEASE, 0, 0)),
          "the end calls each range with MMN_RELEASE and its whole range, unregistered");
    ncalls = 0;
    mmn_notify(&mm_a, MMN_UNMAP, VA, VA + 8 * P);
    check(ncalls == 0, "after the end no range is called");
}

/* A device object of two pages above the counted frames. */
#define DEV_FRAME (FRAMES + 16u)

static int dev_obj;

/** The device's frame for page `pgoff`. */
static int dev_page(void *obj, uint32_t pgoff, uintptr_t *phys)
{
    (void)obj;
    if (pgoff >= 4) {
        return -1;
    }
    *phys = (uintptr_t)(DEV_FRAME + pgoff) * PAGE_SIZE;
    return 0;
}

/** Nothing. */
static void dev_put(void *obj)
{
    (void)obj;
}

static const struct vm_device_ops dev_ops = {dev_page, dev_put, NULL, 0};

/** A fork's write-protection, a write's copy and a device's taking back of
 * its pages are told. */
static void paging_rows(void)
{
    struct mmn_range ra, rd;
    vma_t vma;

    check(vm_insert(&mm_a.vm, VA, VA + 4 * P, RW, VM_ANON) == 0 &&
              resolve_held(&mm_a, VA, 1, ~(uintptr_t)0) == 1 &&
              resolve_held(&mm_a, VA + P, 1, ~(uintptr_t)0) == 1,
          "two pages of a private region are written");
    k_memset(&vma, 0, sizeof(vma));
    vma.end = 4 * P;
    vma.prot = RW;
    vma.type = VM_SHARED;
    check(vm_device_map(&vma, &dev_ops, &dev_obj, VM_PROT_UC) == 0 &&
              vm_insert_file(&mm_a.vm, VD, VD + 4 * P, vma.prot, vma.type, vma.file, vma.pgoff) ==
                  0,
          "a device's four pages are mapped");
    vm_file_put(vma.file);
    check(resolve_held(&mm_a, VD + 2 * P, 1, ~(uintptr_t)0) == 1, "a device page faults in");

    mmn_register(&mm_a, &ra, VA, VA + 4 * P, &ops, NULL);
    mmn_register(&mm_a, &rd, VD, VD + 4 * P, &ops, NULL);
    ncalls = 0;
    check(paging_fork_copy(&mm_a, &dir_b) == 0 && vm_copy(&mm_b.vm, &mm_a.vm) == 0, "fork copies");
    check(ncalls == 1 && had(0, &ra, MMN_PROTECT, VA, VA + 4 * P),
          "the fork tells the private region's write-protection, and not the shared device's");
    ncalls = 0;
    check(resolve_held(&mm_a, VA + P, 1, ~(uintptr_t)0) == 1 &&
              had(0, &ra, MMN_COW, VA + P, VA + 2 * P) && ncalls == 1,
          "a write to a shared page copies it and tells its page");
    check(resolve_held(&mm_b, VA, 1, ~(uintptr_t)0) == 1 && ncalls == 1,
          "a copy in the child tells the parent's range nothing");
    check(resolve_held(&mm_a, VA, 1, ~(uintptr_t)0) == 1 && ncalls == 1 &&
              !(pte_of(&dir_a, VA) & PTE_COW),
          "a write to a page now the parent's alone makes no copy and tells nothing");

    ncalls = 0;
    paging_zap_device(&dev_obj, 1, 2);
    check(ncalls == 1 && had(0, &rd, MMN_TRUNCATE, VD + P, VD + 3 * P) &&
              pte_of(&dir_a, VD + 2 * P) == 0,
          "a device's taking back of two pages tells their range and unmaps them");
    mmn_release(&mm_a);
}

/* A shrinker that holds frames and frees what it is asked for. */
struct holder {
    struct mem_shrinker s; /* first, so that the shrinker is the record */
    uintptr_t frames[256];
    uint32_t held;
    uint32_t asked, last_want;
    int last_sleep, once;
};

static struct holder sh1, sh2;

/** Free up to `want` frames of those held; with `once`, unregister. */
static uint32_t shrink(struct mem_shrinker *s, uint32_t want, int may_sleep)
{
    struct holder *h = (struct holder *)s;
    uint32_t n = 0;

    h->asked++;
    h->last_want = want;
    h->last_sleep = may_sleep;
    while (n < want && h->held > 0) {
        free_frame(h->frames[--h->held]);
        n++;
    }
    if (h->once) {
        mem_shrinker_unregister(s);
    }
    return n;
}

/** Fill `h` with `n` frames. */
static void hold(struct holder *h, uint32_t n)
{
    while (h->held < n) {
        h->frames[h->held++] = alloc_frame();
    }
}

/** Take frames until fewer than the low mark are free; they go in `taken`,
 * whose count is returned. */
static uint32_t drain(uintptr_t *taken, uint32_t most)
{
    uint32_t n = 0;

    while (n < most && frame_free_count() >= frame_total_count() / 16u) {
        taken[n++] = alloc_frame();
    }
    return n;
}

/** Reclaim asks the shrinkers, the last registered first, for what the
 * caches did not give, until the high mark; above the low mark none. */
static void shrinker_rows(void)
{
    static uintptr_t taken[FRAMES];
    uint32_t n, i;
    size_t high = frame_total_count() / 8u;

    sh1.s.shrink = shrink;
    sh2.s.shrink = shrink;
    mem_shrinker_register(&sh1.s);
    mem_shrinker_register(&sh2.s);
    hold(&sh1, 200);
    hold(&sh2, 8);
    mem_balance();
    check(sh1.asked == 0 && sh2.asked == 0, "above the low mark no shrinker is asked");

    n = drain(taken, FRAMES);
    check(frame_free_count() < frame_total_count() / 16u, "free frames are below the low mark");
    logged = 0;
    irq_on = in_call = 1;
    mem_balance();
    check(sh2.asked == 1 && sh2.held == 0 && sh2.last_sleep == 1 && sh1.asked == 1 &&
              sh1.last_want == sh2.last_want - 8,
          "the last registered is asked first, and the other for the rest, from a call that may"
          " sleep");
    check(frame_free_count() >= high && logged == 1,
          "reclaim stops at the high mark and says what it got");

    for (i = 0; i < n; i++) {
        free_frame(taken[i]);
    }
    hold(&sh1, 200);
    hold(&sh2, 8);
    sh2.once = 1;
    n = drain(taken, FRAMES);
    irq_on = 0;
    sh1.asked = sh2.asked = 0;
    mem_balance();
    check(sh2.asked == 1 && sh2.last_sleep == 0 && sh1.asked == 1 && sh1.last_sleep == 0,
          "from a fault no shrinker may sleep");
    for (i = 0; i < n; i++) {
        free_frame(taken[i]);
    }
    n = drain(taken, FRAMES);
    sh1.asked = sh2.asked = 0;
    mem_balance();
    check(sh2.asked == 0 && sh1.asked >= 1, "a shrinker that unregistered itself is asked no more");
    for (i = 0; i < n; i++) {
        free_frame(taken[i]);
    }
    mem_shrinker_unregister(&sh1.s);
    mem_shrinker_unregister(&sh1.s);
    while (sh1.held > 0) {
        free_frame(sh1.frames[--sh1.held]);
    }
    while (sh2.held > 0) {
        free_frame(sh2.frames[--sh2.held]);
    }
    n = drain(taken, FRAMES);
    sh1.asked = 0;
    mem_balance();
    check(sh1.asked == 0, "an unregistered shrinker is asked no more");
    for (i = 0; i < n; i++) {
        free_frame(taken[i]);
    }
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

/** Run every row; 0 when each passes, 1 otherwise. */
/* ---- faults that let the regions' lock go across a fill ---- */

#define VF (WIN_BASE + 2 * SPAN)
#define VZ (WIN_BASE + 3 * SPAN)

static int race_fills, race_inner;

/**
 * The fill of a file whose first read lets a second fault of the same page
 * run meanwhile, as another thread's would while the first sleeps in the
 * disk with the regions' lock let go: the second backs the page first.
 */
static int race_fill(struct vm_file *f, uint32_t pgoff, uintptr_t phys)
{
    (void)f;
    (void)pgoff;
    (void)phys;
    if (race_fills++ == 0) {
        race_inner = resolve_held(&mm_a, VF, 0, ~(uintptr_t)0);
    }
    return 0;
}

static struct vm_file race_file = {1, race_fill, NULL, NULL, NULL, NULL, NULL};

static int zobj, zap_calls;

/** The device's page, whose first call takes the object's pages back from
 * every space, as a driver's move of the page would on another thread
 * between the call and the fault's mapping. */
static int zap_page(void *obj, uint32_t pgoff, uintptr_t *phys)
{
    (void)obj;
    if (zap_calls++ == 0) {
        paging_zap_device(&zobj, 0, 4);
    }
    *phys = (uintptr_t)(DEV_FRAME + 2u + pgoff) * PAGE_SIZE;
    return 0;
}

static const struct vm_device_ops zap_ops = {zap_page, dev_put, NULL, 0};

/** Two faults of one page interleaved across a file's fill map the page
 * once, the later finding it backed and giving its own frame back; and a
 * fault whose device's page was taken back during its call maps nothing
 * from that call and asks again. */
static void lock_rows(void)
{
    uint32_t free_before = (uint32_t)frame_free_count();
    uint32_t zaps;
    pte_t e;
    vma_t vma;
    int rc;

    check(vm_insert_file(&mm_a.vm, VF, VF + 2 * P, VM_PROT_READ, VM_FILE, &race_file, 0) == 0,
          "a file's two pages are mapped");
    rc = resolve_held(&mm_a, VF, 0, ~(uintptr_t)0);
    e = pte_of(&dir_a, VF);
    check(rc == 1 && race_inner == 1 && race_fills == 2 && (e & PTE_P),
          "two faults of one page across the fill both resolve, the page present");
    check(frame_free_count() == free_before - 1u && frame_word(PTE_ADDR(e)) == 1u,
          "the page is mapped once, and the frame of the fault that lost is free again");
    check(!mmap_held(&mm_a, 0), "no hold of the regions' lock is left");

    k_memset(&vma, 0, sizeof(vma));
    vma.end = 4 * P;
    vma.prot = RW;
    vma.type = VM_SHARED;
    check(vm_device_map(&vma, &zap_ops, &zobj, VM_PROT_UC) == 0 &&
              vm_insert_file(&mm_a.vm, VZ, VZ + 4 * P, vma.prot, vma.type, vma.file, vma.pgoff) ==
                  0,
          "a device's four pages are mapped");
    vm_file_put(vma.file);
    zaps = mm_a.zaps;
    rc = resolve_held(&mm_a, VZ, 1, ~(uintptr_t)0);
    check(rc == 1 && zap_calls == 2 && mm_a.zaps == zaps + 1u &&
              PTE_ADDR(pte_of(&dir_a, VZ)) == (uintptr_t)(DEV_FRAME + 2u) * PAGE_SIZE,
          "a page taken back during the device's call is asked for again before it is mapped");
    check(!mmap_held(&mm_a, 0), "no hold of the regions' lock is left after the zap");
}

/* ---- a device object mapped whole at its first fault ---- */

/* Four regions of 70 pages of the whole object, which `device_map_rest`
 * maps in a batch of 64 and one of 6, and its region of two pages in A and
 * B for the zap walk. */
#define WPAGES 70u
#define W1     (VF + 8 * P)
#define W2     (VF + 96 * P)
#define W3     (VF + 192 * P)
#define W4     (VF + 288 * P)
#define WW     (VZ + 16 * P)

/* What the object's page does: there are pages below `whole_end`, and the
 * call for page `whole_at` zaps the object (`WHOLE_ZAP`) or unmaps the last
 * four pages of W4 (`WHOLE_CUT`), once. */
enum { WHOLE_NONE, WHOLE_ZAP, WHOLE_CUT };

static int wobj, whole_do;
static uint32_t whole_end, whole_at, whole_calls;

/** The whole object's frame for page `pgoff`, past the counted frames, and
 * the change `whole_do` names on the call for `whole_at`. */
static int whole_page(void *obj, uint32_t pgoff, uintptr_t *phys)
{
    (void)obj;
    whole_calls++;
    if (pgoff >= whole_end) {
        return -1;
    }
    if (whole_do != WHOLE_NONE && pgoff == whole_at) {
        if (whole_do == WHOLE_ZAP) {
            paging_zap_device(&wobj, 0, 1024);
        } else {
            mmap_write_lock(&mm_a);
            vm_remove(&mm_a.vm, W4 + (WPAGES - 4) * P, W4 + WPAGES * P);
            unmap_user_range(&dir_a, W4 + (WPAGES - 4) * P, W4 + WPAGES * P);
            mmap_unlock(&mm_a);
        }
        whole_do = WHOLE_NONE;
    }
    *phys = (uintptr_t)(DEV_FRAME + 8u + pgoff) * PAGE_SIZE;
    return 0;
}

static const struct vm_device_ops whole_ops = {whole_page, dev_put, NULL, 1};

/** How many of the `n` pages from `va` in `dir` are mapped to the whole
 * object's frames for pages `pgoff` on. */
static uint32_t whole_mapped(page_dir_t *dir, uintptr_t va, uint32_t pgoff, uint32_t n)
{
    uint32_t i, got = 0;

    for (i = 0; i < n; i++) {
        pte_t e = pte_of(dir, va + i * P);

        if ((e & PTE_P) && PTE_ADDR(e) == (uintptr_t)(DEV_FRAME + 8u + pgoff + i) * PAGE_SIZE) {
            got++;
        }
    }
    return got;
}

extern struct mm *host_busy;
extern void (*host_wait)(struct mm *mm);

static int wait_rc;

/** What A's thread does while the walk waits for B's lock with the big lock
 * free: it touches the object again, which a pass that zapped A already
 * cannot see. */
static void refault_a(struct mm *mm)
{
    (void)mm;
    wait_rc = resolve_held(&mm_a, WW, 1, ~(uintptr_t)0);
}

/**
 * The first fault of a whole object maps its region; an object that ends
 * early leaves the rest to their faults; a zap during a batch's calls maps
 * nothing of the batch; and a region cut short during the calls maps only
 * what remains of it.
 */
static void whole_rows(void)
{
    uint32_t zaps;
    vma_t vma;

    k_memset(&vma, 0, sizeof(vma));
    vma.end = WPAGES * P;
    vma.prot = RW;
    vma.type = VM_SHARED;
    check(vm_device_map(&vma, &whole_ops, &wobj, VM_PROT_UC) == 0 &&
              vm_insert_file(&mm_a.vm, W1, W1 + WPAGES * P, vma.prot, vma.type, vma.file, 0) == 0 &&
              vm_insert_file(&mm_a.vm, W2, W2 + WPAGES * P, vma.prot, vma.type, vma.file, 0) == 0 &&
              vm_insert_file(&mm_a.vm, W3, W3 + WPAGES * P, vma.prot, vma.type, vma.file, 0) == 0 &&
              vm_insert_file(&mm_a.vm, W4, W4 + WPAGES * P, vma.prot, vma.type, vma.file, 0) == 0 &&
              vm_insert_file(&mm_a.vm, WW, WW + 2 * P, vma.prot, vma.type, vma.file, 0) == 0 &&
              vm_insert_file(&mm_b.vm, WW, WW + 2 * P, vma.prot, vma.type, vma.file, 0) == 0,
          "a whole object is mapped in four regions of 70 pages, and two pages in A and B");
    vm_file_put(vma.file);

    whole_end = 1024;
    whole_calls = 0;
    check(resolve_held(&mm_a, W1 + 5 * P, 0, ~(uintptr_t)0) == 1 &&
              whole_mapped(&dir_a, W1, 0, WPAGES) == WPAGES && whole_calls == 1 + WPAGES,
          "the first fault maps the whole region, in two batches after its own page");

    whole_end = 40;
    check(resolve_held(&mm_a, W2 + 3 * P, 0, ~(uintptr_t)0) == 1 &&
              whole_mapped(&dir_a, W2, 0, 40) == 40 && pte_of(&dir_a, W2 + 40 * P) == 0,
          "an object that ends early has its pages mapped up to its end and no further");
    check(resolve_held(&mm_a, W2 + 50 * P, 0, ~(uintptr_t)0) == 0 &&
              pte_of(&dir_a, W2 + 50 * P) == 0,
          "and a page past its end faults to nothing");
    if (current_proc) {
        current_proc->bus_fault = 0;
    }

    whole_end = 1024;
    whole_do = WHOLE_ZAP;
    whole_at = 66;
    zaps = mm_a.zaps;
    check(resolve_held(&mm_a, W3 + P, 1, ~(uintptr_t)0) == 1 && whole_do == WHOLE_NONE &&
              mm_a.zaps == zaps + 1u && whole_mapped(&dir_a, W3, 0, WPAGES) == 0 &&
              whole_mapped(&dir_a, W1, 0, WPAGES) == 0,
          "a zap during the second batch's calls unmaps the first and maps nothing of the"
          " second");
    check(!mmap_held(&mm_a, 0), "no hold of the regions' lock is left after the zap");

    whole_do = WHOLE_CUT;
    whole_at = 65;
    zaps = mm_a.zaps;
    check(resolve_held(&mm_a, W4, 1, ~(uintptr_t)0) == 1 && whole_do == WHOLE_NONE &&
              mm_a.zaps == zaps && whole_mapped(&dir_a, W4, 0, WPAGES - 4) == WPAGES - 4 &&
              vm_find(&mm_a.vm, W4 + (WPAGES - 4) * P) == NULL &&
              pte_of(&dir_a, W4 + (WPAGES - 4) * P) == 0,
          "a region cut short during the second batch's calls has what remains of it mapped");
    check(!mmap_held(&mm_a, 0), "no hold of the regions' lock is left after the cut");
}

/** A zap walk that waits for one space's lock zaps again a space that
 * faulted the object back in during the wait. */
static void walk_rows(void)
{
    uint32_t zaps, zaps_b;

    check(resolve_held(&mm_a, WW, 1, ~(uintptr_t)0) == 1 &&
              resolve_held(&mm_b, WW, 1, ~(uintptr_t)0) == 1 &&
              whole_mapped(&dir_a, WW, 0, 2) == 2 && whole_mapped(&dir_b, WW, 0, 2) == 2,
          "A and B map the object's two pages");
    zaps = mm_a.zaps;
    zaps_b = mm_b.zaps;
    wait_rc = -2;
    host_busy = &mm_b;
    host_wait = refault_a;
    paging_zap_device(&wobj, 0, 1024);
    host_wait = NULL;
    check(wait_rc == 1 && host_busy == NULL && mm_a.zaps == zaps + 2u && mm_b.zaps == zaps_b + 1u &&
              pte_of(&dir_a, WW) == 0 && pte_of(&dir_a, WW + P) == 0 && pte_of(&dir_b, WW) == 0,
          "a walk that waits for B's lock zaps A again, which faulted the object back in"
          " meanwhile");
    check(!mmap_held(&mm_a, 0) && !mmap_held(&mm_b, 0),
          "no hold of the regions' lock is left after the walk");
}

/** Run the rows, or with `--lock-only` the faults that let the regions' lock
 * go and the walk that waits for one, alone. */
int main(int argc, char **argv)
{
    model_start(NAME);
    if (argc == 2 && __builtin_strcmp(argv[1], "--lock-only") == 0) {
        lock_rows();
        whole_rows();
        walk_rows();
        return model_end();
    }
    notify_rows();
    paging_rows();
    shrinker_rows();
    lock_rows();
    whole_rows();
    walk_rows();
    return model_end();
}
