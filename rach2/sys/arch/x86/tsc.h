/*
 * tsc.h: the time-stamp counter read in order, the check at each
 * processor's start that the counters agree, and the processor number
 * `rdtscp` gives, for the clock the vDSO reads (../x86_64/vclock.c).
 */
#ifndef _TSC_H_
#define _TSC_H_

#include "../../lib/kint.h"

/** The time-stamp counter, read after every earlier instruction has
 * finished and before any later one starts (`lfence` on both sides), so
 * that a reading cannot be taken early or late against the loads and stores
 * around it. */
uint64_t tsc_ordered(void);

/** Whether the counter runs at one rate whatever the processor's clock and
 * power state do (`cpuid` leaf 0x80000007, EDX bit 8). */
int tsc_invariant(void);

/**
 * The TSC's counts in a tick as KVM states them at the first call, or 0.
 * Under KVM with its second clock MSR and the stable bit
 * (KVM_FEATURE_CLOCKSOURCE2 and KVM_FEATURE_CLOCKSOURCE_STABLE_BIT), the
 * clock page KVM fills for the boot processor gives the rate, and its
 * PVCLOCK_TSC_STABLE_BIT says that the counter is one clock on every
 * processor, as Linux's kvm-clock reads it (arch/x86/kernel/kvmclock.c:
 * 116-120, 317-324; pvclock.c:27-37). Where the page gives both, it stays
 * registered and `tsc_kvm_poll` follows it; otherwise it is given up. 0
 * elsewhere, with either feature or the page's bit clear, or once
 * `rach2.notsc` is on the command line. The answer is kept from the first
 * call; the boot processor makes it with interrupts off.
 */
uint32_t tsc_kvm_counts_per_tick(void);

/**
 * Whether KVM has written the page that `tsc_kvm_counts_per_tick` keeps
 * with another rate or stable bit since the last call, which takes them:
 * then 1, with the counts in a tick into `*counts` and the bit into
 * `*stable`. A rate that does not fit 32 bits keeps the last one and reads
 * as a withdrawn bit. 0, with nothing written, when nothing changed or no
 * page is kept. One processor at a time calls it, the clock's tick's.
 */
int tsc_kvm_poll(uint32_t *counts, int *stable);

/** Whether the page `tsc_kvm_counts_per_tick` keeps has withdrawn its
 * stable bit as of the last `tsc_kvm_poll`, so that the TSC is no longer
 * known to be one clock on every processor. 0 when no page is kept. */
int tsc_kvm_withdrawn(void);

/** Whether KVM states the TSC's rate and that it is one clock on every
 * processor, as of the last `tsc_kvm_poll`. */
int tsc_kvm_stable(void);

/** Whether the processor has `rdtscp` (`cpuid` leaf 0x80000001, EDX bit
 * 27), which reads the counter and IA32_TSC_AUX together. */
int tsc_has_rdtscp(void);

/**
 * Set IA32_TSC_AUX on the running processor to `cpu`, the number `rdtscp`
 * then gives the vDSO's getcpu, as Linux's encoding has it: the node above
 * bit 12, here always 0, and the processor below. Nothing without
 * `rdtscp`.
 */
void tsc_cpu_setup(int cpu);

/* What `tsc_sync_source` found. */
#define TSC_SYNC_NONE       0 /* not checked: no rate was measured */
#define TSC_SYNC_AGREE      1 /* no reading fell behind another's */
#define TSC_SYNC_BEHIND     2 /* a reading fell behind; see `*behind` */
#define TSC_SYNC_UNFINISHED 3 /* a wait timed out and the round was aborted */

/**
 * Begin the round of the check for processor `cpu`, above 0, before it is
 * sent INIT: a processor of an earlier round, however late, then changes
 * nothing of this one's.
 */
void tsc_sync_open(int cpu);

/**
 * The boot processor's and a starting processor's halves of the check that
 * their counters agree, each called once per started processor, the
 * starting one's from its entry once the boot processor has seen it alive
 * (smpboot.c), both in the round `tsc_sync_open` began. The two meet, then
 * take turns under one lock for two ticks' worth of counts, each reading its
 * counter and storing it where the other reads it; a reading below the last
 * one stored, whoever stored it, is a counter behind another's. The boot
 * processor then waits for the other to finish its turns. Either waits for
 * the other at most 100 ticks at each step; one that times out aborts the
 * round, and the other leaves at its next look. The check is skipped while
 * `tsc_counts_per_tick` (pit.h) is 0. `tsc_sync_source` returns a
 * TSC_SYNC_ value and sets `*behind` to the round's worst shortfall in
 * counts; a round TSC_SYNC_BEHIND or TSC_SYNC_UNFINISHED fails `tsc_synced`.
 */
int tsc_sync_source(int cpu, uint64_t *behind);
void tsc_sync_target(int cpu);

/** Whether every processor started so far passed the check: 1 when they
 * all did or only the boot processor runs. `*cpu` and `*behind` get the
 * first that did not and by how many counts it fell behind, 0 when its
 * round was aborted before any reading fell behind. */
int tsc_synced(int *cpu, uint64_t *behind);

#endif /* _TSC_H_ */
