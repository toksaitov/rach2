/*
 * kvmclock.c: KVM's paravirtual clock as its guest keeps it, as Linux's
 * arch/x86/kernel/kvmclock.c and pvclock.c keep kvm-clock: a clock page
 * for each processor, which KVM fills, the reading that converts the TSC
 * through a page, the wall clock KVM states, and the page's stopped flag.
 */

#include "kvmclock.h"

#include "kcpu.h"
#include "tsc.h"
#include "../arch.h"
#include "../../cmdline.h"
#include "../../timer.h"
#include "../../watchdog.h"
#include "../../lib/kirq.h"
#include "../../lib/spinlock.h"
#include "../../proc/percpu.h"

/* KVM's paravirtual features (Documentation/virt/kvm/x86/cpuid.rst): the
 * clock by its first pair of MSRs, by its second, and the promise that a
 * page's PVCLOCK_TSC_STABLE_BIT means what it says. */
#define KVM_FEATURE_CLOCKSOURCE            (1u << 0)
#define KVM_FEATURE_CLOCKSOURCE2           (1u << 3)
#define KVM_FEATURE_CLOCKSOURCE_STABLE_BIT (1u << 24)

/* The clock's MSRs (Documentation/virt/kvm/x86/msr.rst): each pair takes
 * the physical address of the wall clock's record and of a processor's
 * clock page, the page's with bit 0 set to turn it on. */
#define MSR_KVM_WALL_CLOCK      0x11u
#define MSR_KVM_SYSTEM_TIME     0x12u
#define MSR_KVM_WALL_CLOCK_NEW  0x4B564D00u
#define MSR_KVM_SYSTEM_TIME_NEW 0x4B564D01u

/* A page's flags (arch/x86/include/asm/pvclock-abi.h): KVM's clock is one
 * clock on every processor; and the host stopped the guest for a while. */
#define PVCLOCK_TSC_STABLE_BIT 0x01u
#define PVCLOCK_GUEST_STOPPED  0x02u

#define NSEC_PER_SEC 1000000000u

/* KVM's pvclock_vcpu_time_info, 32 bytes, which may not cross a page. */
struct kvmclock_page {
    volatile uint32_t version; /* odd while KVM writes the rest */
    uint32_t pad0;
    volatile uint64_t tsc_timestamp;
    volatile uint64_t system_time;
    volatile uint32_t tsc_to_system_mul; /* nanoseconds per count, by 2^32 */
    volatile signed char tsc_shift;      /* the count's shift before `mul` */
    volatile uint8_t flags;
    uint8_t pad[2];
};

/* KVM's pvclock_wall_clock, 12 bytes: real time when the guest's clock
 * read 0. */
struct kvmclock_wall {
    volatile uint32_t version; /* odd while KVM writes the rest */
    volatile uint32_t sec;
    volatile uint32_t nsec;
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
static uint64_t kvmclock_room[PAGE_SIZE / sizeof(uint64_t)] __attribute__((aligned(4096)));
typedef char kvmclock_room_fits[NR_CPUS * sizeof(struct kvmclock_page) <= PAGE_SIZE ? 1 : -1];

/* The wall clock's record, which KVM writes once for each write of the
 * MSR. */
static struct kvmclock_wall wall_record __attribute__((aligned(16)));

/*
 * The clock as the kernel follows it: whether the pages are kept,
 * registered on each processor; the MSRs KVM offers, 0 for none; whether
 * a page's stable bit counts, Linux's valid_flags; the rate and stable bit
 * the tick last took from a page; and the rate at the first reading, which
 * `kvmclock_counts_per_tick` keeps. Each processor registers its own page
 * before it counts a tick.
 */
static int kept;
static uint32_t msr_system_time;
static uint32_t msr_wall_clock;
static int stable_valid;
static uint32_t counts_now;
static int stable_now;
static uint32_t boot_counts;

/*
 * The latest time a reading without the stable bit gave, on any processor,
 * Linux's last_value (pvclock.c:47): no such reading is earlier. Linux
 * exchanges it without a lock; the i686 has no 64-bit exchange before the
 * Pentium's, so a lock is held for the compare, on this path alone.
 */
static uint64_t last_value;
static struct spinlock last_lock = SPINLOCK_INIT_NAMED("kvmclock_last_lock");

/** Processor `cpu`'s page. */
static struct kvmclock_page *kvmclock_of(int cpu)
{
    return (struct kvmclock_page *)kvmclock_room + cpu;
}

/** Give KVM processor `cpu`'s page, the running one's, as Linux's
 * kvm_register_clock does (arch/x86/kernel/kvmclock.c:167-177). */
static void kvmclock_register(int cpu)
{
    struct kvmclock_page *p = kvmclock_of(cpu);
    uint64_t phys = (uint64_t)virt_to_phys(p);

    kcpu_wrmsr(msr_system_time, (uint32_t)phys | 1u, (uint32_t)(phys >> 32));
}

/** `num` / `den`, by the bit, without i686's absent libgcc divide. */
static uint64_t kvmclock_div(uint64_t num, uint32_t den)
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
static uint32_t kvmclock_counts(uint32_t mul, int shift)
{
    uint64_t counts;

    if (mul == 0 || shift < -31 || shift > 31) {
        return 0;
    }
    counts = kvmclock_div((uint64_t)TICK_NS << 32, mul);
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

/** `delta` counts of the TSC in nanoseconds at a page's `mul` and `shift`,
 * Linux's pvclock_scale_delta (arch/x86/include/asm/pvclock.h:44-81): the
 * count shifted, then times `mul` by 2^-32, from the product of each half. */
static uint64_t kvmclock_scale(uint64_t delta, uint32_t mul, int shift)
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
static uint64_t kvmclock_convert(uint64_t tsc, uint64_t stamp, uint64_t system, uint32_t mul,
                                 int shift)
{
    return system + (tsc > stamp ? kvmclock_scale(tsc - stamp, mul, shift) : 0u);
}

/** One reading of the page at `p` with the TSC between two equal even
 * versions, as Linux's __pvclock_clocksource_read makes it
 * (arch/x86/kernel/pvclock.c:75-79): the version into `*version`, the TSC
 * and KVM's clock at it into `s->tsc` and `s->ns` (`kvmclock_convert`),
 * the counts in a tick its rate gives into `s->counts`, 0 for a rate that
 * does not fit, and into `s->stable` its PVCLOCK_TSC_STABLE_BIT, which
 * counts only with KVM_FEATURE_CLOCKSOURCE_STABLE_BIT, as Linux masks a
 * page's flags by its valid_flags (pvclock.c:55-64). Returns the page's
 * flags as KVM wrote them. */
static int kvmclock_page_read(struct kvmclock_page *p, uint32_t *version, struct kvmclock_sample *s)
{
    uint64_t stamp, system;
    uint32_t mul;
    int shift, flags;

    do {
        *version = p->version;
        arch_dma_fence();
        stamp = p->tsc_timestamp;
        system = p->system_time;
        mul = p->tsc_to_system_mul;
        shift = p->tsc_shift;
        flags = p->flags;
        s->tsc = tsc_ordered();
        arch_dma_fence();
    } while ((*version & 1u) != 0 || *version != p->version);
    s->ns = kvmclock_convert(s->tsc, stamp, system, mul, shift);
    s->counts = kvmclock_counts(mul, shift);
    s->stable = stable_valid && (flags & PVCLOCK_TSC_STABLE_BIT) != 0;
    return flags;
}

/** Clear the stopped flag of the page at `p`, the running processor's, and
 * touch the soft-lockup detector, as Linux does wherever it finds the flag
 * (pvclock_touch_watchdogs, pvclock.c): the stretch the host kept the guest
 * stopped is not a lockup. With interrupts off. */
static void kvmclock_resumed(struct kvmclock_page *p)
{
    p->flags &= (uint8_t)~PVCLOCK_GUEST_STOPPED;
    touch_softlockup_watchdog();
}

/**
 * Hold a reading without the stable bit, `ns`, to the latest one any
 * processor gave, and make it the latest when it is later, as Linux's
 * __pvclock_clocksource_read does with last_value (pvclock.c:86-110): the
 * processors' pages, which KVM writes one at a time, may then disagree by
 * a little, and a reading on one must not be earlier than a reading
 * already given on another. Returns the time to give. Any context but an
 * NMI's.
 */
static uint64_t kvmclock_hold(uint64_t ns)
{
    uint32_t flags = spin_lock_irqsave(&last_lock);

    if (ns <= last_value) {
        ns = last_value;
    } else {
        last_value = ns;
    }
    spin_unlock_irqrestore(&last_lock, flags);
    return ns;
}

/**
 * The pages stay registered when the first reading finds a rate, so that
 * KVM goes on writing them: KVM recomputes the clock and the stable bit and
 * writes a page again (Linux's arch/x86/kvm/x86.c:2604 and 2875), and
 * Linux's reader takes the page as it is at every reading (pvclock.c:75).
 * The stable bit decides nothing here, as in Linux's kvmclock_init, which
 * registers kvm-clock with it or without it. The page is given up at once
 * when KVM has not written it or its rate does not fit.
 */
uint32_t kvmclock_counts_per_tick(void)
{
    static int asked;
    struct kvmclock_sample s;
    uint32_t features, version;

    if (asked) {
        return boot_counts;
    }
    asked = 1;
    if (kcmdline_has("no-kvmclock") || kcmdline_has("rach2.notsc") || !kcpu_msr_present()) {
        return 0;
    }
    features = kcpu_kvm_features();
    if (features & KVM_FEATURE_CLOCKSOURCE2) {
        msr_system_time = MSR_KVM_SYSTEM_TIME_NEW;
        msr_wall_clock = MSR_KVM_WALL_CLOCK_NEW;
    } else if (features & KVM_FEATURE_CLOCKSOURCE) {
        msr_system_time = MSR_KVM_SYSTEM_TIME;
        msr_wall_clock = MSR_KVM_WALL_CLOCK;
    } else {
        return 0;
    }
    stable_valid = (features & KVM_FEATURE_CLOCKSOURCE_STABLE_BIT) != 0;
    kvmclock_register(0);
    (void)kvmclock_page_read(kvmclock_of(0), &version, &s);
    if (version == 0 || s.counts == 0) {
        kcpu_wrmsr(msr_system_time, 0, 0);
        msr_system_time = 0;
        msr_wall_clock = 0;
        return 0;
    }
    counts_now = s.counts;
    stable_now = s.stable;
    boot_counts = s.counts;
    kept = 1;
    return s.counts;
}

int kvmclock_kept(void)
{
    return kvmclock_counts_per_tick() != 0;
}

int kvmclock_stable(void)
{
    return kvmclock_counts_per_tick() != 0 && stable_now;
}

int kvmclock_vsyscall(void)
{
    return !kcmdline_has("no-kvmclock-vsyscall");
}

void kvmclock_cpu_setup(int cpu)
{
    if (kept && cpu > 0 && cpu < NR_CPUS) {
        kvmclock_register(cpu);
    }
}

void kvmclock_cpu_stop(void)
{
    if (kept) {
        kcpu_wrmsr(msr_system_time, 0, 0);
    }
}

/** The running processor's page, as Linux's this_cpu_pvti gives it
 * (kvmclock.c:74-82), or the boot processor's while KVM has not written the
 * running one's, before `kvmclock_cpu_setup` registers it; NULL when none
 * is kept. */
static struct kvmclock_page *kvmclock_here(void)
{
    struct kvmclock_page *p;
    int cpu = arch_cpu_id();

    if (!kept) {
        return 0;
    }
    if (cpu < 0 || cpu >= NR_CPUS) {
        return kvmclock_of(0);
    }
    p = kvmclock_of(cpu);
    return p->version != 0 ? p : kvmclock_of(0);
}

/**
 * The kernel's reading of the page at `p`, Linux's
 * __pvclock_clocksource_read whole (pvclock.c): the page read with the
 * TSC into `*s` (`kvmclock_page_read`), its stopped flag cleared where the
 * host set it (`kvmclock_resumed`), a rate that does not fit read as a
 * clear stable bit, and a time without the bit held to the latest given
 * (`kvmclock_hold`). With interrupts off.
 */
static void kvmclock_kernel_read(struct kvmclock_page *p, struct kvmclock_sample *s)
{
    uint32_t version;

    if (kvmclock_page_read(p, &version, s) & PVCLOCK_GUEST_STOPPED) {
        kvmclock_resumed(p);
    }
    if (s->counts == 0) {
        s->stable = 0;
    }
    if (!s->stable) {
        s->ns = kvmclock_hold(s->ns);
    }
}

int kvmclock_sample(struct kvmclock_sample *s)
{
    struct kvmclock_page *p = kvmclock_here();

    if (!p) {
        return KVMCLOCK_NONE;
    }
    kvmclock_kernel_read(p, s);
    if (s->counts == 0) {
        s->counts = counts_now; /* a rate that does not fit keeps the last */
    }
    if (s->counts == counts_now && s->stable == stable_now) {
        return KVMCLOCK_SAME;
    }
    counts_now = s->counts;
    stable_now = s->stable;
    return KVMCLOCK_CHANGED;
}

int kvmclock_now(uint64_t *tsc, uint64_t *ns)
{
    struct kvmclock_page *p = kvmclock_here();
    struct kvmclock_sample s;

    if (!p) {
        return 0;
    }
    kvmclock_kernel_read(p, &s);
    *tsc = s.tsc;
    *ns = s.ns;
    return 1;
}

int kvmclock_read(struct kvmclock_sample *s)
{
    struct kvmclock_page *p = kvmclock_here();

    if (!p) {
        return 0;
    }
    kvmclock_kernel_read(p, s);
    return 1;
}

/**
 * Linux's vread_pvclock (arch/x86/include/asm/vdso/gettimeofday.h:
 * 197-232): the version made even (pvclock_read_begin, asm/pvclock.h:
 * 22-29) and the bit checked before the counter is read, the counter
 * converted by the page's fields read in the same pass, and the reading
 * made again when the version moved meanwhile or was odd
 * (pvclock_read_retry, pvclock.h:31-36); the time loses its top bit, as
 * vread_pvclock's `ret & S64_MAX`. Without
 * KVM_FEATURE_CLOCKSOURCE_STABLE_BIT the bit says nothing and nothing is
 * read, as Linux then never gives its vDSO the page
 * (kvm_setup_vsyscall_timeinfo, kvmclock.c:241-262).
 */
int kvmclock_boot_read(uint64_t *tsc, uint64_t *ns)
{
    struct kvmclock_page *p = kvmclock_of(0);
    uint64_t stamp, system;
    uint32_t version, mul;
    int shift;

    if (!kept || !stable_valid) {
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
    *ns = kvmclock_convert(*tsc, stamp, system, mul, shift) & 0x7FFFFFFFFFFFFFFFull;
    return 1;
}

/** Linux's pvclock_read_flags (arch/x86/kernel/pvclock.c:55-64), of the
 * boot processor's page. */
int kvmclock_boot_withdrawn(void)
{
    struct kvmclock_page *p = kvmclock_of(0);
    uint32_t version;
    int flags;

    if (!kept) {
        return 0;
    }
    do {
        version = p->version & ~1u;
        arch_dma_fence();
        flags = p->flags;
        arch_dma_fence();
    } while (version != p->version);
    return !stable_valid || !(flags & PVCLOCK_TSC_STABLE_BIT);
}

uintptr_t kvmclock_page_frame(void)
{
    return virt_to_phys(kvmclock_room);
}

/**
 * Linux's kvm_check_and_clear_guest_paused (kvmclock.c): whether the host
 * stopped the guest since the last call on this processor, by its page's
 * PVCLOCK_GUEST_STOPPED, which KVM sets when the VM's owner says so
 * (KVM_KVMCLOCK_CTRL) and this clears (`kvmclock_resumed`), unless a
 * reading of the clock met the flag first and cleared it. 0 where no page
 * is kept. With interrupts off.
 */
int arch_guest_paused(void)
{
    struct kvmclock_page *p = kvmclock_here();

    if (!p || !(p->flags & PVCLOCK_GUEST_STOPPED)) {
        return 0;
    }
    kvmclock_resumed(p);
    return 1;
}

/**
 * KVM writes the record when the MSR is written, so it is whole when the
 * write returns; the version is still read on both sides, as Linux's
 * pvclock_read_wallclock reads it. The time since the guest's start is
 * the kernel's reading of this processor's page
 * (pvclock_clocksource_read). A record KVM left at 0 is no time.
 */
int kvmclock_wallclock(int64_t *sec, uint32_t *nsec)
{
    struct kvmclock_wall *w = &wall_record;
    uint64_t phys = (uint64_t)virt_to_phys(w);
    uint64_t tsc, ns, rem;
    uint32_t version, wsec, wnsec, flags;
    int read;

    if (!kvmclock_kept()) {
        return 0;
    }
    kcpu_wrmsr(msr_wall_clock, (uint32_t)phys, (uint32_t)(phys >> 32));
    do {
        version = w->version;
        arch_dma_fence();
        wsec = w->sec;
        wnsec = w->nsec;
        arch_dma_fence();
    } while ((version & 1u) != 0 || version != w->version);
    if (version == 0) {
        return 0;
    }
    flags = kirq_save();
    read = kvmclock_now(&tsc, &ns);
    kirq_restore(flags);
    if (!read) {
        return 0;
    }
    ns += (uint64_t)wsec * NSEC_PER_SEC + wnsec;
    *sec = (int64_t)tick_div64(ns, NSEC_PER_SEC, &rem);
    *nsec = (uint32_t)rem;
    return 1;
}
