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
 * The check's shared state. `sync_state` holds the round, the number of the
 * processor being started, above SYNC_ROUND_SHIFT and the round's flags below
 * it; each processor changes it by compare-and-exchange against the round it
 * belongs to, so one left over from an earlier round changes nothing. The
 * last reading either stored and the round's worst shortfall are kept under
 * `sync_lock`. The verdict is kept across rounds.
 */
#define SYNC_SOURCE      1u /* the boot processor has arrived */
#define SYNC_TARGET      2u /* the starting processor has arrived */
#define SYNC_TARGET_DONE 4u /* the starting processor has taken its turns */
#define SYNC_ABORT       8u /* a wait timed out; both leave */
#define SYNC_ROUND_SHIFT 4

static volatile unsigned int sync_state;
static volatile uint64_t sync_last;
static volatile uint64_t sync_round_behind;
static struct spinlock sync_lock;
static volatile int sync_failed_cpu;
static volatile uint64_t sync_behind;

/** Whether `state` is round `cpu`'s and not aborted. */
static int sync_live(unsigned int state, int cpu)
{
    return state >> SYNC_ROUND_SHIFT == (unsigned int)cpu && !(state & SYNC_ABORT);
}

/** Set `flag` in round `cpu`'s state; 0, with nothing set, when the round is
 * over or aborted. */
static int sync_mark(int cpu, unsigned int flag)
{
    unsigned int old;

    do {
        old = sync_state;
        if (!sync_live(old, cpu)) {
            return 0;
        }
    } while (arch_atomic_cmpxchg(&sync_state, old, old | flag) != old);
    return 1;
}

/** Wait for the other processor to set `flag` in round `cpu`'s state, at
 * most SYNC_MEET_TICKS ticks; 1 once it has. On a timeout this one aborts
 * the round, so that the other, whenever it next looks, leaves too; 0 then,
 * and when the other aborted first. */
static int sync_wait(int cpu, unsigned int flag)
{
    uint64_t start = tsc_ordered();
    uint64_t limit = SYNC_MEET_TICKS * (uint64_t)tsc_counts_per_tick();
    unsigned int old;

    for (;;) {
        old = sync_state;
        if (!sync_live(old, cpu)) {
            return 0;
        }
        if (old & flag) {
            return 1;
        }
        if (tsc_ordered() - start >= limit) {
            if (arch_atomic_cmpxchg(&sync_state, old, old | SYNC_ABORT) == old) {
                return 0;
            }
            continue;
        }
        arch_relax();
    }
}

/** Take turns with the other processor for `window` counts of this one's
 * counter, or until round `cpu` is aborted, and record the worst a reading
 * fell below the last one stored. */
static void sync_turns(int cpu, uint64_t window)
{
    uint64_t start = tsc_ordered(), now, prev;
    int live;

    do {
        spin_lock(&sync_lock);
        live = sync_live(sync_state, cpu);
        now = tsc_ordered();
        if (live) {
            prev = sync_last;
            sync_last = now;
            if (now < prev && prev - now > sync_round_behind) {
                sync_round_behind = prev - now;
            }
        }
        spin_unlock(&sync_lock);
    } while (live && now - start < window);
}

void tsc_sync_open(int cpu)
{
    spin_lock(&sync_lock);
    sync_state = (unsigned int)cpu << SYNC_ROUND_SHIFT;
    sync_last = 0;
    sync_round_behind = 0;
    spin_unlock(&sync_lock);
}

int tsc_sync_source(int cpu, uint64_t *behind)
{
    uint64_t window = 2u * (uint64_t)tsc_counts_per_tick();
    int done, verdict;

    *behind = 0;
    if (window == 0) {
        return TSC_SYNC_NONE;
    }
    done = sync_mark(cpu, SYNC_SOURCE) && sync_wait(cpu, SYNC_TARGET);
    if (done) {
        sync_turns(cpu, window);
        done = sync_wait(cpu, SYNC_TARGET_DONE);
    }
    spin_lock(&sync_lock);
    *behind = sync_round_behind;
    spin_unlock(&sync_lock);
    if (!done) {
        verdict = TSC_SYNC_UNFINISHED;
    } else if (*behind != 0) {
        verdict = TSC_SYNC_BEHIND;
    } else {
        return TSC_SYNC_AGREE;
    }
    if (sync_failed_cpu == 0) {
        sync_failed_cpu = cpu;
        sync_behind = *behind;
    }
    return verdict;
}

void tsc_sync_target(int cpu)
{
    uint64_t window = 2u * (uint64_t)tsc_counts_per_tick();

    if (window == 0 || !sync_mark(cpu, SYNC_TARGET) || !sync_wait(cpu, SYNC_SOURCE)) {
        return;
    }
    sync_turns(cpu, window);
    sync_mark(cpu, SYNC_TARGET_DONE);
}

int tsc_synced(int *cpu, uint64_t *behind)
{
    *cpu = sync_failed_cpu;
    *behind = sync_behind;
    return sync_failed_cpu == 0;
}
