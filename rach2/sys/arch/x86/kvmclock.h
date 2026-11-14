/*
 * kvmclock.h: KVM's paravirtual clock as its guest keeps it, Linux's
 * kvm-clock: a clock page for each processor, the kernel's reading of it,
 * the wall clock KVM states, and the page the vDSO maps (kvmclock.c).
 */
#ifndef _KVMCLOCK_H_
#define _KVMCLOCK_H_

#include "../../lib/kint.h"

/**
 * The TSC's counts in a tick as KVM states them at the first call, or 0.
 * The first call decides whether the kernel keeps KVM's clock, as Linux's
 * kvmclock_init does (arch/x86/kernel/kvmclock.c in 6.12): under
 * KVM with KVM_FEATURE_CLOCKSOURCE2, or else KVM_FEATURE_CLOCKSOURCE and
 * its older pair of MSRs, the boot processor gives KVM its clock page, and
 * where KVM has written a rate there the pages are kept, with the stable
 * bit or without it. Each other processor then registers its own
 * (`kvmclock_cpu_setup`). 0 elsewhere, under `no-kvmclock`, Linux's option,
 * or `rach2.notsc`, and where the page states no rate that fits 32 bits.
 * The answer is kept from the first call; the boot processor makes it with
 * interrupts off.
 */
uint32_t kvmclock_counts_per_tick(void);

/** Whether KVM's clock pages are kept (`kvmclock_counts_per_tick`). */
int kvmclock_kept(void);

/** Whether KVM states that its clock is one clock on every processor, as of
 * the last `kvmclock_sample`: KVM_FEATURE_CLOCKSOURCE_STABLE_BIT, without
 * which a page's bit says nothing, as Linux's `valid_flags`
 * (pvclock_set_flags, arch/x86/kernel/pvclock.c), and the page's
 * PVCLOCK_TSC_STABLE_BIT. */
int kvmclock_stable(void);

/** Whether the vDSO may read KVM's clock: 0 under `no-kvmclock-vsyscall`,
 * Linux's option (kvmclock.c's `kvmclock_vsyscall`). */
int kvmclock_vsyscall(void);

/**
 * Give KVM processor `cpu`'s clock page, on that processor, where the
 * pages are kept, as Linux's kvm_setup_secondary_clock does at each
 * processor's start (kvmclock.c:190-193). Nothing for the boot processor,
 * whose page `kvmclock_counts_per_tick` gave. With interrupts off, before
 * the processor's first tick.
 */
void kvmclock_cpu_setup(int cpu);

/**
 * Take the running processor's clock page back from KVM, which then writes
 * it no more, as Linux's kvmclock_disable does when a processor goes
 * offline and at a shutdown or a restart (kvmclock.c; kvm.c's
 * kvm_guest_cpu_offline). The page keeps what KVM last wrote, so a reading
 * after it still gives a time. Any context.
 */
void kvmclock_cpu_stop(void);

/* One reading of the running processor's clock page with the TSC. */
struct kvmclock_sample {
    uint64_t tsc;    /* the TSC at the reading */
    uint64_t ns;     /* KVM's clock at it in nanoseconds */
    uint32_t counts; /* the TSC's counts in a tick at the page's rate */
    int stable;      /* the page's stable bit, where the feature gives it */
};

/* What `kvmclock_sample` found. */
#define KVMCLOCK_NONE    0 /* no page is kept */
#define KVMCLOCK_SAME    1 /* the rate and the bit are the ones last taken */
#define KVMCLOCK_CHANGED 2 /* KVM changed the rate or the bit, now taken */

/**
 * Read the running processor's clock page into `*s`, as Linux's kvm-clock
 * reads each processor's own (kvm_clock_read, kvmclock.c:74-82), or the
 * boot processor's while KVM has not written this one's. A reading without
 * the stable bit is never earlier than one any processor made before it
 * (Linux's last_value, pvclock.c:86-110). A rate that does not fit 32 bits
 * keeps the last one and reads as a clear bit. When the rate or the bit
 * changed, they become the ones `kvmclock_stable` gives. One processor at
 * a time calls it, the one counting the tick, with interrupts off.
 */
int kvmclock_sample(struct kvmclock_sample *s);

/** The TSC into `*tsc` and KVM's clock at it into `*ns`, read as
 * `kvmclock_sample` reads them, taking nothing from the page; 0, with
 * nothing written, when no page is kept. With interrupts off. */
int kvmclock_now(uint64_t *tsc, uint64_t *ns);

/** The page `kvmclock_now` reads, read whole into `*s` in one pass with
 * the TSC, taking nothing from it; a rate that does not fit 32 bits reads
 * as a clear bit. 0, with nothing written, when no page is kept. With
 * interrupts off. */
int kvmclock_read(struct kvmclock_sample *s);

/**
 * The TSC into `*tsc` and KVM's clock at it into `*ns`, converted through
 * the boot processor's clock page, the one the vDSO reads, in one pass
 * while the page has its stable bit, as Linux's vDSO reads that page inside
 * each reading (vread_pvclock, arch/x86/include/asm/vdso/gettimeofday.h:
 * 197-232); 0, with nothing that may be used, when the bit is clear or no
 * page is kept. Any processor and any context may call it.
 */
int kvmclock_boot_read(uint64_t *tsc, uint64_t *ns);

/** Whether the boot processor's clock page, the one every reader of the
 * vDSO's clock tests, lacks its stable bit now, read from the page itself;
 * 0 when no page is kept. Any context may call it. */
int kvmclock_boot_withdrawn(void);

/** The physical address of the page that holds the processors' clock
 * pages, the boot processor's first, which the vDSO maps; the page holds
 * nothing else. */
uintptr_t kvmclock_page_frame(void);

/**
 * Real time now as KVM states it, in `*sec` seconds since the epoch and
 * `*nsec` nanoseconds: the wall clock at the guest's start, which KVM
 * writes when given a place for it with MSR_KVM_WALL_CLOCK_NEW or its older
 * MSR, plus KVM's clock, as Linux's kvm_get_wallclock reads it
 * (kvmclock.c; pvclock_read_wallclock, pvclock.c). 1, or 0
 * with nothing written where no page is kept or KVM wrote no time. With
 * interrupts on or off; one caller at a time, the boot's.
 */
int kvmclock_wallclock(int64_t *sec, uint32_t *nsec);

#endif /* _KVMCLOCK_H_ */
