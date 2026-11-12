/*
 * vgpushm_env.h: the kernel around rach2/sys/virtio/shmem.c for a host
 * program of one processor, which includes the transport through this file:
 * a region of REGION_PAGES static pages with made-up frames, a clock that
 * moves a tick for each pass of a sleep, references to the region's frames
 * counted, a memory object of SH_PAGES pages in a directory of leaves, four
 * eventfds whose references are counted and which are never rung or hooked,
 * a hook that runs once interrupts are let in with vshm_lock free, and a
 * sleep that lets the includer's model device look at the region until its
 * condition holds. The includer defines REGION_PAGES, `check` and
 * `model_step`.
 */
#ifndef VGPUSHM_ENV_H
#define VGPUSHM_ENV_H

#include <stdarg.h>

extern int printf(const char *format, ...);
extern int vsnprintf(char *out, unsigned long size, const char *fmt, va_list ap);
extern char *strstr(const char *haystack, const char *needle);

#include "virtio/shmem.c"
#include "udelay.h"

/* The region of REGION_PAGES pages, which the includer sets: the control
 * page, the ring area's two pages and the heap. Frame i is FRAME(i), in
 * descending order. */
#define FRAME(i) ((uintptr_t)0x10000000u + (uintptr_t)(REGION_PAGES - 1u - (i)) * 0x3000u)
#define FOREIGN  ((uintptr_t)0x0fff0000u)

static char last_log[256];
static uint64_t fake_ticks;
static uint8_t region[REGION_PAGES][VGS_PAGE] __attribute__((aligned(4096)));
static int32_t frame_refs[REGION_PAGES]; /* references beyond the attach's */
static uint32_t unrefs, unmaps;

process_t proc_a;
struct percpu cpu_table[NR_CPUS] = {{.current = &proc_a}};

/* The includer's: a failed invariant, and one look of the device at the
 * region, which each pass of the thread's sleep takes. */
static void check(int ok, const char *what);
static void model_step(void);

/* ------------------------------------------------------------------------
 * The kernel's services, as one processor with no interrupts sees them
 */

/** Keep the record as the last line logged. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    va_list ap;

    (void)level;
    (void)subsystem;
    va_start(ap, format);
    vsnprintf(last_log, sizeof(last_log), format, ap);
    va_end(ap);
}

/** No wait is needed: the model answers a reset at once. */
void k_udelay(uint32_t usec)
{
    (void)usec;
}

/** One processor keeps its own order. */
void arch_dma_fence(void)
{}

/** No interrupts to turn off. */
uint32_t kirq_save(void)
{
    return 0;
}

/* A row's stand-in for another processor, run once at the first point
 * after it is set where interrupts are let in with vshm_lock free, then
 * cleared. */
static void (*irq_on_hook)(void);

/** Nothing to put back; run `irq_on_hook` when it is set and vshm_lock is
 * free. */
void kirq_restore(uint32_t flags)
{
    void (*hook)(void) = irq_on_hook;

    (void)flags;
    if (hook && !vshm_lock.locked) {
        irq_on_hook = NULL;
        hook();
    }
}

/** The fake clock, which each pass of a sleep moves by a tick. */
uint64_t tick_count(void)
{
    return fake_ticks;
}

/** The region's page of the made-up frame `phys`, or REGION_PAGES. */
static uint32_t frame_page(uintptr_t phys)
{
    uint32_t i;

    for (i = 0; i < REGION_PAGES; i++) {
        if (FRAME(i) == phys) {
            return i;
        }
    }
    return REGION_PAGES;
}

/** Count a reference to one of the region's frames. */
void frame_ref(uintptr_t phys)
{
    uint32_t i = frame_page(phys);

    check(i < REGION_PAGES, "a reference taken to a frame outside the region");
    if (i < REGION_PAGES) {
        frame_refs[i]++;
    }
}

/** Drop a reference; the attach's own, held in `frames`, count in `unrefs`. */
uint32_t frame_unref(uintptr_t phys)
{
    uint32_t i = frame_page(phys);

    check(i < REGION_PAGES, "a reference dropped to a frame outside the region");
    if (i < REGION_PAGES && frame_refs[i] > 0) {
        frame_refs[i]--;
    } else {
        unrefs++;
    }
    return 1;
}

/** Count the window's removal. */
void mmio_unmap(uintptr_t va, size_t size)
{
    check(va == (uintptr_t)region && size == sizeof(region), "the window unmapped whole");
    unmaps++;
}

/*
 * A memory object whose fill adopts up to SH_PAGES pages, which is all a row
 * gives one, into leaf 0 of a directory of SH_LEAVES leaves of SHMEM_LEAF
 * frames, as mm/shmem.c keeps its table. The directory reaches one leaf
 * once a page is adopted; a row may move pages into the far leaf, the
 * directory's last, by hand, leaving the leaves between it and leaf 0
 * missing. `sh_lookups` counts the frames asked for.
 */
#define SH_PAGES  8u
#define SH_LEAVES 3u
static uintptr_t sh_frames[SHMEM_LEAF], sh_far[SHMEM_LEAF];
static uintptr_t *sh_dir[SH_LEAVES];
static uint32_t sh_lookups;

/** The frame at `pgoff`, or 0, as the directory of `sh` holds it. */
uintptr_t shmem_resident(const struct shmem *sh, uint32_t pgoff)
{
    sh_lookups++;
    if (pgoff >= sh->npages || !sh->leaves[pgoff / SHMEM_LEAF]) {
        return 0;
    }
    return sh->leaves[pgoff / SHMEM_LEAF][pgoff % SHMEM_LEAF];
}

/** Take `phys` at `pgoff` with the caller's reference, and add the
 * mapping's, as mm/shmem.c does; the directory reaches leaf 0 after. */
uintptr_t shmem_adopt(struct shmem *sh, uint32_t pgoff, uintptr_t phys)
{
    if (pgoff >= SH_PAGES || sh_frames[pgoff]) {
        frame_unref(phys);
        return 0;
    }
    sh_dir[0] = sh_frames;
    sh->leaves = sh_dir;
    if (sh->nleaves == 0) {
        sh->nleaves = 1;
        sh->npages = SHMEM_LEAF;
    }
    sh_frames[pgoff] = phys;
    frame_ref(phys);
    return phys;
}

/** Drop every page's reference and the directory. */
void shmem_free(struct shmem *sh)
{
    uint32_t l, i;

    for (l = 0; sh->leaves && l < sh->nleaves; l++) {
        for (i = 0; sh->leaves[l] && i < SHMEM_LEAF; i++) {
            if (sh->leaves[l][i]) {
                frame_unref(sh->leaves[l][i]);
                sh->leaves[l][i] = 0;
            }
        }
        sh_dir[l] = NULL;
    }
    sh->leaves = NULL;
    sh->nleaves = sh->npages = 0;
}

/** No eventfd is rung: the rows poll. */
void eventfd_signal(struct eventfd *ev)
{
    (void)ev;
    check(0, "a doorbell rung while the transport polls");
}

/* The eventfds descriptors 0 to 3 are open on, for an attach's doorbells,
 * and the references taken on each. */
static char bell_fds[4];
static unsigned bell_refs[4];

/** A reference to descriptor `fd`'s eventfd, or NULL with -EBADF past 3. */
struct eventfd *eventfd_get(int fd, int32_t *err)
{
    if (fd < 0 || fd > 3) {
        *err = -EBADF;
        return NULL;
    }
    bell_refs[fd]++;
    return (struct eventfd *)(void *)&bell_fds[fd];
}

/** Drop a reference eventfd_get took. */
void eventfd_put(struct eventfd *ev)
{
    int i;

    for (i = 0; i < 4 && ev != (struct eventfd *)(void *)&bell_fds[i]; i++) {
    }
    check(i < 4 && bell_refs[i] > 0, "an eventfd put that was never taken");
    if (i < 4 && bell_refs[i] > 0) {
        bell_refs[i]--;
    }
}

/** No eventfd is hooked. */
int eventfd_hook_remove(struct eventfd *ev, struct eventfd_hook *h, uint64_t *cnt)
{
    (void)ev;
    (void)h;
    (void)cnt;
    check(0, "a hook removed that was never added");
    return 0;
}

/** Nobody else waits. */
void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
}

/** The thread's sleep: the model runs and the clock moves a tick a pass,
 * until the condition holds. */
int wq_wait_event_idle(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    while (!cond(arg)) {
        model_step();
        fake_ticks++;
    }
    return 0;
}

/** One thread: there is no one to give way to. */
void sched_service_point(void)
{}

#endif
