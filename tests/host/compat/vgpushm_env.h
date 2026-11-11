/*
 * vgpushm_env.h: the kernel around rach2/sys/virtio/shmem.c for a host
 * program of one processor, which includes the transport through this file:
 * a region of REGION_PAGES static pages with made-up frames, a clock that
 * moves a tick for each pass of a sleep, references to the region's frames
 * counted, a memory object of SH_PAGES pages, no eventfds, and a sleep that
 * lets the includer's model device look at the region until its condition
 * holds. The includer defines REGION_PAGES, `check` and `model_step`.
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

/** Nothing to put back. */
void kirq_restore(uint32_t flags)
{
    (void)flags;
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

/* A memory object of up to 8 pages, which is all a row gives one. */
#define SH_PAGES 8u
static uintptr_t sh_frames[SH_PAGES];

/** The frame at `pgoff`, or 0. */
uintptr_t shmem_resident(const struct shmem *sh, uint32_t pgoff)
{
    (void)sh;
    return pgoff < SH_PAGES ? sh_frames[pgoff] : 0;
}

/** Take `phys` at `pgoff` with the caller's reference, and add the
 * mapping's, as mm/shmem.c does. */
uintptr_t shmem_adopt(struct shmem *sh, uint32_t pgoff, uintptr_t phys)
{
    (void)sh;
    if (pgoff >= SH_PAGES || sh_frames[pgoff]) {
        frame_unref(phys);
        return 0;
    }
    sh_frames[pgoff] = phys;
    frame_ref(phys);
    return phys;
}

/** Drop every page's reference. */
void shmem_free(struct shmem *sh)
{
    uint32_t i;

    (void)sh;
    for (i = 0; i < SH_PAGES; i++) {
        if (sh_frames[i]) {
            frame_unref(sh_frames[i]);
            sh_frames[i] = 0;
        }
    }
}

/** No eventfd is held: the rows poll. */
void eventfd_signal(struct eventfd *ev)
{
    (void)ev;
    check(0, "a doorbell rung while the transport polls");
}

/** No eventfd is held. */
void eventfd_put(struct eventfd *ev)
{
    (void)ev;
    check(0, "an eventfd put that was never taken");
}

/** No eventfd is held. */
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

#endif
