/*
 * tsc.c: the time-stamp counter read in order, the check that the
 * processors' counters agree, and IA32_TSC_AUX, as tsc.h describes them.
 */

#include "tsc.h"

#include "kcpu.h"
#include "pit.h"
#include "../arch.h"
#include "../../lib/spinlock.h"

/* The register `rdtscp` returns in ECX beside the counter. */
#define MSR_TSC_AUX 0xC0000103u

/* How long either processor waits for the other to meet it, in ticks of
 * the counter's measured rate; the boot processor has already seen the
 * other alive. The wait reads the counter, not the 8254, which the two
 * would otherwise latch at once. */
#define SYNC_MEET_TICKS 100u

uint64_t tsc_ordered(void)
{
    uint32_t lo, hi;

    __asm__ __volatile__("lfence\n\trdtsc\n\tlfence" : "=a"(lo), "=d"(hi) : : "memory");
    return ((uint64_t)hi << 32) | lo;
}

int tsc_invariant(void)
{
    uint32_t a, b, c, d;

    kcpu_id(0x80000000u, &a, &b, &c, &d);
    if (a < 0x80000007u) {
        return 0;
    }
    kcpu_id(0x80000007u, &a, &b, &c, &d);
    return (d & (1u << 8)) != 0;
}

int tsc_has_rdtscp(void)
{
    uint32_t a, b, c, d;

    kcpu_id(0x80000000u, &a, &b, &c, &d);
    if (a < 0x80000001u) {
        return 0;
    }
    kcpu_id(0x80000001u, &a, &b, &c, &d);
    return (d & (1u << 27)) != 0;
}

void tsc_cpu_setup(int cpu)
{
    if (tsc_has_rdtscp()) {
        kcpu_wrmsr(MSR_TSC_AUX, (uint32_t)cpu & 0xFFFu, 0);
    }
}

/*
 * The check's shared state: the processors that have arrived and that have
 * finished, the last reading either stored, and the lock they take turns
 * under. One processor starts at a time, so one copy serves each in turn.
 * The verdict is kept across processors.
 */
static volatile unsigned int sync_arrived;
static volatile unsigned int sync_done;
static volatile uint64_t sync_last;
static struct spinlock sync_lock;
static volatile int sync_failed_cpu;
static volatile uint64_t sync_behind;

/** Add one to `*count`, which both processors change. */
static void sync_count(volatile unsigned int *count)
{
    unsigned int old;

    do {
        old = *count;
    } while (arch_atomic_cmpxchg(count, old, old + 1u) != old);
}

/** Take turns with the other processor for `window` counts of this one's
 * counter, and record against the starting processor `cpu` the worst a
 * reading fell below the last one stored. */
static void sync_turns(int cpu, uint64_t window)
{
    uint64_t start = tsc_ordered(), now, prev;

    do {
        spin_lock(&sync_lock);
        prev = sync_last;
        now = tsc_ordered();
        sync_last = now;
        if (now < prev && prev - now > sync_behind) {
            sync_behind = prev - now;
            if (sync_failed_cpu == 0) {
                sync_failed_cpu = cpu;
            }
        }
        spin_unlock(&sync_lock);
    } while (now - start < window);
}

/** Arrive at the meeting and wait for the other processor, at most
 * SYNC_MEET_TICKS ticks; 1 when both are there. */
static int sync_meet(void)
{
    uint64_t start = tsc_ordered();
    uint64_t limit = SYNC_MEET_TICKS * (uint64_t)tsc_counts_per_tick();

    sync_count(&sync_arrived);
    while (sync_arrived < 2u) {
        if (tsc_ordered() - start >= limit) {
            return 0;
        }
        arch_relax();
    }
    return 1;
}

void tsc_sync_source(int cpu)
{
    uint64_t window = 2u * (uint64_t)tsc_counts_per_tick();

    if (window == 0) {
        return;
    }
    sync_last = 0;
    sync_done = 0;
    if (!sync_meet()) {
        if (sync_failed_cpu == 0) {
            sync_failed_cpu = cpu;
        }
        sync_arrived = 0;
        return;
    }
    sync_turns(cpu, window);
    sync_count(&sync_done);
    while (sync_done < 2u) {
        arch_relax();
    }
    sync_arrived = 0;
}

void tsc_sync_target(int cpu)
{
    uint64_t window = 2u * (uint64_t)tsc_counts_per_tick();

    if (window == 0) {
        return;
    }
    if (!sync_meet()) {
        return;
    }
    sync_turns(cpu, window);
    sync_count(&sync_done);
}

int tsc_synced(int *cpu, uint64_t *behind)
{
    *cpu = sync_failed_cpu;
    *behind = sync_behind;
    return sync_failed_cpu == 0;
}
