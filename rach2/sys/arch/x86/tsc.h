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

/**
 * The boot processor's and a starting processor's halves of the check that
 * their counters agree, each called once per started processor, the
 * starting one's from its entry once the boot processor has seen it alive
 * (smpboot.c). The two meet, then take turns under one lock for two ticks'
 * worth of counts, each reading its counter and storing it where the other
 * reads it; a reading below the last one stored, whoever stored it, is a
 * counter behind another's. The check is skipped while `tsc_counts_per_tick`
 * (pit.h) is 0, and either waits for the other at most 100 ticks.
 */
void tsc_sync_source(int cpu);
void tsc_sync_target(int cpu);

/** Whether every processor started so far passed the check: 1 when they
 * all did or only the boot processor runs. `*cpu` and `*behind` get the
 * first that did not and by how many counts it fell behind. */
int tsc_synced(int *cpu, uint64_t *behind);

#endif /* _TSC_H_ */
