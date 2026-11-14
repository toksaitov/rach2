/*
 * tsc.c: the time-stamp counter read in order, the check that the
 * processors' counters agree, and IA32_TSC_AUX, as tsc.h describes them.
 */

#include "tsc.h"

#include "kcpu.h"
#include "pit.h"
#include "../arch.h"
#include "../../cmdline.h"
#include "../../timer.h"
#include "../../lib/spinlock.h"
#include "../../proc/percpu.h"

/* The register `rdtscp` returns in ECX beside the counter. */
#define MSR_TSC_AUX 0xC0000103u

/* How long either processor waits for the other to meet it, in ticks of
 * the counter's measured rate; the boot processor has already seen the
 * other alive. The wait reads the counter, not the 8254, which the two
 * would otherwise latch at once. */
#define SYNC_MEET_TICKS MS_TO_TICKS(100u)

uint64_t tsc_ordered(void)
{
    uint32_t lo, hi;

    /* lfence as bytes 0f ae e8: i686 tcc has no mnemonic for it. */
    __asm__ __volatile__(".byte 0x0f, 0xae, 0xe8\n\trdtsc\n\t.byte 0x0f, 0xae, 0xe8"
                         : "=a"(lo), "=d"(hi)
                         :
                         : "memory");
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

/* KVM's paravirtual features (Documentation/virt/kvm/x86/cpuid.rst) and
 * the clock MSR the second of them names (msr.rst). */
#define KVM_FEATURE_CLOCKSOURCE2           (1u << 3)
#define KVM_FEATURE_CLOCKSOURCE_STABLE_BIT (1u << 24)
#define MSR_KVM_SYSTEM_TIME_NEW            0x4B564D01u
#define PVCLOCK_TSC_STABLE_BIT             0x01u

/* KVM's pvclock_vcpu_time_info, 32 bytes, which may not cross a page. */
struct kvm_clock_page {
    volatile uint32_t version; /* odd while KVM writes the rest */
    uint32_t pad0;
    volatile uint64_t tsc_timestamp;
    volatile uint64_t system_time;
    volatile uint32_t tsc_to_system_mul; /* nanoseconds per count, by 2^32 */
    volatile signed char tsc_shift;      /* the count's shift before `mul` */
    volatile uint8_t flags;
    uint8_t pad[2];
};

/*
 * A page for each processor, 32 bytes each, in a page of memory of their
 * own, which the vDSO maps read-only for the boot processor's, as Linux's
 * hv_clock_boot is a page that holds nothing else (arch/x86/kernel/
 * kvmclock.c:44-50), whose first entry kvm_init_platform gives the vDSO
 * (pvclock_set_pvti_cpu0_va, kvmclock.c:315) and vvar_fault maps
 * (arch/x86/entry/vdso/vma.c:185-191). NR_CPUS of them fit; an array size
 * of -1 fails the compile.
 */
static uint64_t kvm_clock_room[PAGE_SIZE / sizeof(uint64_t)] __attribute__((aligned(4096)));
typedef char kvm_clock_room_fits[NR_CPUS * sizeof(struct kvm_clock_page) <= PAGE_SIZE ? 1 : -1];

/** Processor `cpu`'s page. */
static struct kvm_clock_page *kvm_clock_of(int cpu)
{
    return (struct kvm_clock_page *)kvm_clock_room + cpu;
}

/** Give KVM processor `cpu`'s page, the running one's, with
 * MSR_KVM_SYSTEM_TIME_NEW, as Linux's kvm_register_clock does
 * (arch/x86/kernel/kvmclock.c:167-177). */
static void kvm_clock_register(int cpu)
{
    struct kvm_clock_page *p = kvm_clock_of(cpu);
    uint64_t phys = (uint64_t)virt_to_phys(p);

    kcpu_wrmsr(MSR_KVM_SYSTEM_TIME_NEW, (uint32_t)phys | 1u, (uint32_t)(phys >> 32));
}

/** `num` / `den`, by the bit, without i686's absent libgcc divide. */
static uint64_t tsc_div(uint64_t num, uint32_t den)
{
    uint64_t q = 0, rem = 0;
    int bit;

    for (bit = 63; bit >= 0; --bit) {
        rem = (rem << 1) | ((num >> bit) & 1u);
        if (rem >= den) {
            rem -= den;
            q |= (uint64_t)1 << bit;
        }
    }
    return q;
}

/** The counts in a tick for a clock page's `tsc_to_system_mul` and
 * `tsc_shift`, Linux's pvclock_tsc_khz (pvclock.c:27-37) for a tick in place
 * of a millisecond: a tick's nanoseconds by 2^32, divided by `mul`, then
 * shifted against `shift`. 0 for a rate that does not fit 32 bits. */
static uint32_t kvm_clock_counts(uint32_t mul, int shift)
{
    uint64_t counts;

    if (mul == 0 || shift < -31 || shift > 31) {
        return 0;
    }
    counts = tsc_div((uint64_t)TICK_NS << 32, mul);
    if (shift < 0) {
        if (counts > (0xFFFFFFFFull >> -shift)) {
            return 0;
        }
        counts <<= -shift;
    } else {
        counts >>= shift;
    }
    return counts > 0xFFFFFFFFull ? 0u : (uint32_t)counts;
}

/*
 * The pages as the kernel follows them: whether they are kept, registered
 * on each processor; the rate and stable bit the tick last took from one;
 * and the rate at the first reading, which `tsc_kvm_counts_per_tick` keeps.
 * Each processor registers its own before it counts a tick.
 */
static int kvm_kept;
static uint32_t kvm_counts;
static int kvm_stable;
static uint32_t kvm_boot_counts;

/** `delta` counts of the TSC in nanoseconds at a page's `mul` and `shift`,
 * Linux's pvclock_scale_delta (arch/x86/include/asm/pvclock.h:44-81): the
 * count shifted, then times `mul` by 2^-32, from the product of each half. */
static uint64_t kvm_clock_scale(uint64_t delta, uint32_t mul, int shift)
{
    if (shift < -31 || shift > 31) {
        return 0;
    }
    if (shift < 0) {
        delta >>= -shift;
    } else {
        delta <<= shift;
    }
    return (delta >> 32) * mul + (((delta & 0xFFFFFFFFu) * mul) >> 32);
}

/** KVM's clock in nanoseconds at the TSC's count `tsc` by a page's stamp
 * `stamp`, its system time `system` and its rate, Linux's
 * __pvclock_read_cycles (arch/x86/include/asm/pvclock.h:84-90); a count
 * before the stamp, as another processor's can be by a few counts, is the
 * stamp's time. Every reader of a page converts through it, the vDSO's
 * code as well, so that the kernel and the vDSO read one clock. */
static uint64_t kvm_clock_convert(uint64_t tsc, uint64_t stamp, uint64_t system, uint32_t mul,
                                  int shift)
{
    return system + (tsc > stamp ? kvm_clock_scale(tsc - stamp, mul, shift) : 0u);
}

/** One reading of the page at `p` with the TSC between two equal even
 * versions, as Linux's __pvclock_clocksource_read makes it
 * (arch/x86/kernel/pvclock.c:75-79): the version into `*version`, the TSC
 * and KVM's clock at it into `s->tsc` and `s->ns` (`kvm_clock_convert`),
 * the counts in a tick its rate gives into `s->counts`, 0 for a rate that
 * does not fit, and its PVCLOCK_TSC_STABLE_BIT into `s->stable`. */
static void kvm_clock_read(struct kvm_clock_page *p, uint32_t *version, struct tsc_kvm_sample *s)
{
    uint64_t stamp, system;
    uint32_t mul;
    int shift;

    do {
        *version = p->version;
        arch_dma_fence();
        stamp = p->tsc_timestamp;
        system = p->system_time;
        mul = p->tsc_to_system_mul;
        shift = p->tsc_shift;
        s->stable = (p->flags & PVCLOCK_TSC_STABLE_BIT) != 0;
        s->tsc = tsc_ordered();
        arch_dma_fence();
    } while ((*version & 1u) != 0 || *version != p->version);
    s->ns = kvm_clock_convert(s->tsc, stamp, system, mul, shift);
    s->counts = kvm_clock_counts(mul, shift);
}

/**
 * The page stays registered when the first reading takes its rate, so that
 * KVM goes on writing it: KVM recomputes the clock and the stable bit and
 * writes the page again (Linux's arch/x86/kvm/x86.c:2604 and 2875), and
 * Linux's reader takes the page as it is at every reading (pvclock.c:75).
 * Given up at once when the first reading does not take it.
 */
uint32_t tsc_kvm_counts_per_tick(void)
{
    static int asked;
    struct tsc_kvm_sample s;
    uint32_t want = KVM_FEATURE_CLOCKSOURCE2 | KVM_FEATURE_CLOCKSOURCE_STABLE_BIT;
    uint32_t version;

    if (asked) {
        return kvm_boot_counts;
    }
    asked = 1;
    if (kcmdline_has("rach2.notsc") || !kcpu_msr_present() ||
        (kcpu_kvm_features() & want) != want) {
        return 0;
    }
    kvm_clock_register(0);
    kvm_clock_read(kvm_clock_of(0), &version, &s);
    if (version == 0 || !s.stable || s.counts == 0) {
        kcpu_wrmsr(MSR_KVM_SYSTEM_TIME_NEW, 0, 0);
        return 0;
    }
    kvm_counts = s.counts;
    kvm_stable = 1;
    kvm_boot_counts = s.counts;
    kvm_kept = 1;
    return s.counts;
}

/** The running processor's page, as Linux's this_cpu_pvti gives it
 * (kvmclock.c:74-82), or the boot processor's while KVM has not written the
 * running one's, before `tsc_cpu_setup` registers it; NULL when none is
 * kept. */
static struct kvm_clock_page *kvm_clock_here(void)
{
    struct kvm_clock_page *p;
    int cpu = arch_cpu_id();

    if (!kvm_kept) {
        return 0;
    }
    if (cpu < 0 || cpu >= NR_CPUS) {
        return kvm_clock_of(0);
    }
    p = kvm_clock_of(cpu);
    return p->version != 0 ? p : kvm_clock_of(0);
}

int tsc_kvm_sample(struct tsc_kvm_sample *s)
{
    struct kvm_clock_page *p = kvm_clock_here();
    uint32_t version;

    if (!p) {
        return TSC_KVM_NONE;
    }
    kvm_clock_read(p, &version, s);
    if (s->counts == 0) {
        s->counts = kvm_counts; /* a rate that does not fit keeps no clock */
        s->stable = 0;
    }
    if (s->counts == kvm_counts && s->stable == kvm_stable) {
        return TSC_KVM_SAME;
    }
    kvm_counts = s->counts;
    kvm_stable = s->stable;
    return TSC_KVM_CHANGED;
}

int tsc_kvm_now(uint64_t *tsc, uint64_t *ns)
{
    struct kvm_clock_page *p = kvm_clock_here();
    struct tsc_kvm_sample s;
    uint32_t version;

    if (!p) {
        return 0;
    }
    kvm_clock_read(p, &version, &s);
    *tsc = s.tsc;
    *ns = s.ns;
    return 1;
}

int tsc_kvm_read(struct tsc_kvm_sample *s)
{
    struct kvm_clock_page *p = kvm_clock_here();
    uint32_t version;

    if (!p) {
        return 0;
    }
    kvm_clock_read(p, &version, s);
    if (s->counts == 0) {
        s->stable = 0;
    }
    return 1;
}

/**
 * Linux's vread_pvclock (arch/x86/include/asm/vdso/gettimeofday.h:
 * 197-232): the version made even (pvclock_read_begin, asm/pvclock.h:
 * 22-29) and the bit checked before the counter is read, the counter
 * converted by the page's fields read in the same pass, and the reading
 * made again when the version moved meanwhile or was odd
 * (pvclock_read_retry, pvclock.h:31-36); the time loses its top bit, as
 * vread_pvclock's `ret & S64_MAX`.
 */
int tsc_kvm_boot_read(uint64_t *tsc, uint64_t *ns)
{
    struct kvm_clock_page *p = kvm_clock_of(0);
    uint64_t stamp, system;
    uint32_t version, mul;
    int shift;

    if (!kvm_kept) {
        return 0;
    }
    do {
        version = p->version & ~1u;
        arch_dma_fence();
        if (!(p->flags & PVCLOCK_TSC_STABLE_BIT)) {
            return 0;
        }
        stamp = p->tsc_timestamp;
        system = p->system_time;
        mul = p->tsc_to_system_mul;
        shift = p->tsc_shift;
        *tsc = tsc_ordered();
        arch_dma_fence();
    } while (version != p->version);
    *ns = kvm_clock_convert(*tsc, stamp, system, mul, shift) & 0x7FFFFFFFFFFFFFFFull;
    return 1;
}

/** Linux's pvclock_read_flags (arch/x86/kernel/pvclock.c:55-64), of the
 * boot processor's page. */
int tsc_kvm_boot_withdrawn(void)
{
    struct kvm_clock_page *p = kvm_clock_of(0);
    uint32_t version;
    int flags;

    if (!kvm_kept) {
        return 0;
    }
    do {
        version = p->version & ~1u;
        arch_dma_fence();
        flags = p->flags;
        arch_dma_fence();
    } while (version != p->version);
    return !(flags & PVCLOCK_TSC_STABLE_BIT);
}

uintptr_t tsc_kvm_page_frame(void)
{
    return virt_to_phys(kvm_clock_room);
}

int tsc_kvm_stable(void)
{
    return tsc_kvm_counts_per_tick() != 0 && kvm_stable;
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

/** A processor other than the boot one gives KVM its own clock page when
 * the boot one's is kept, as Linux's kvm_setup_secondary_clock does
 * (kvmclock.c:190-193), so that the tick it counts reads its own. */
void tsc_cpu_setup(int cpu)
{
    if (tsc_has_rdtscp()) {
        kcpu_wrmsr(MSR_TSC_AUX, (uint32_t)cpu & 0xFFFu, 0);
    }
    if (kvm_kept && cpu > 0 && cpu < NR_CPUS) {
        kvm_clock_register(cpu);
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
static struct spinlock sync_lock = SPINLOCK_INIT_NAMED("tsc_sync_lock");
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
    uint64_t limit = SYNC_MEET_TICKS * (uint64_t)tsc_clock_per_tick();
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
    uint64_t window = tick_div64((uint64_t)tsc_clock_per_tick() * 2000u, TICK_US, NULL);
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
    uint64_t window = tick_div64((uint64_t)tsc_clock_per_tick() * 2000u, TICK_US, NULL);

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
