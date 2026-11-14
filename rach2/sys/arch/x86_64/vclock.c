/*
 * vclock.c: the clock the x86-64's vDSO reads, kept from the TSC in the
 * data page beside the vDSO's image, and read by the kernel's own clock
 * calls as well while it runs (../../vdso.h).
 */

#include "../../vdso.h"
#include "vdsoimg.h"

#include "../arch.h"
#include "../x86/pit.h"
#include "../x86/refclock.h"
#include "../x86/tsc.h"

#include "../../klog.h"
#include "../../rtc.h"
#include "../../timer.h"
#include "../../lib/kstring.h"
#include "../../lib/spinlock.h"
#include "../../mm/paging.h"
#include "../../proc/percpu.h"
#include "../../proc/sched.h"

/* The struct's offsets, which vdsocode.s reads the page at: an array size of -1
 * fails the compile. TinyCC's build skips the check. */
#if !defined(__TINYC__)
typedef char
    vdso_data_fits[(__builtin_offsetof(struct vdso_data, seq) == VDSO_DATA_SEQ &&
                    __builtin_offsetof(struct vdso_data, mode) == VDSO_DATA_MODE &&
                    __builtin_offsetof(struct vdso_data, cycle_base) == VDSO_DATA_CYCLE_BASE &&
                    __builtin_offsetof(struct vdso_data, mult) == VDSO_DATA_MULT &&
                    __builtin_offsetof(struct vdso_data, shift) == VDSO_DATA_SHIFT &&
                    __builtin_offsetof(struct vdso_data, mono_sec) == VDSO_DATA_MONO_SEC &&
                    __builtin_offsetof(struct vdso_data, mono_nsec) == VDSO_DATA_MONO_NSEC &&
                    __builtin_offsetof(struct vdso_data, real_mult) == VDSO_DATA_REAL_MULT &&
                    __builtin_offsetof(struct vdso_data, real_sec) == VDSO_DATA_REAL_SEC &&
                    __builtin_offsetof(struct vdso_data, real_nsec) == VDSO_DATA_REAL_NSEC &&
                    __builtin_offsetof(struct vdso_data, res_ns) == VDSO_DATA_RES_NS &&
                    __builtin_offsetof(struct vdso_data, getcpu) == VDSO_DATA_GETCPU &&
                    __builtin_offsetof(struct vdso_data, coarse_ns) == VDSO_DATA_COARSE_NS &&
                    __builtin_offsetof(struct vdso_data, raw_mult) == VDSO_DATA_RAW_MULT &&
                    __builtin_offsetof(struct vdso_data, raw_sec) == VDSO_DATA_RAW_SEC &&
                    __builtin_offsetof(struct vdso_data, raw_nsec) == VDSO_DATA_RAW_NSEC &&
                    __builtin_offsetof(struct vdso_data, tz_minuteswest) == VDSO_DATA_TZ_MW &&
                    __builtin_offsetof(struct vdso_data, tz_dsttime) == VDSO_DATA_TZ_DST)
                       ? 1
                       : -1];
#endif

#define NSEC_PER_SEC 1000000000u

/*
 * The steering. Each tick a clock's error against the tick's clock it
 * follows, the tick's less the TSC's, is to be made good over the next
 * 2^SLEW_SHIFT nanoseconds, about 16.8 ms, by a rate that much faster or
 * slower than the one it runs at, the measured rate times the NTP state's
 * for the monotonic clock; the error is cut to SLEW_MAX_NS, so the rate is
 * never more than SLEW_MAX_NS / 2^SLEW_SHIFT, one part in 2000, from that
 * rate. A clock more than STEP_NS behind, as after a stretch the boot
 * processor spent with interrupts off longer than the steering can make
 * good, is stepped forward; one ahead is only slowed, since a step back
 * would make a reading earlier than one before it.
 */
#define SLEW_SHIFT  24
#define SLEW_MAX_NS 8388
#define STEP_NS     1000000u

/*
 * The clock: whether it runs, and whether it stopped, or did not start,
 * because KVM's clock page withdrew its stable bit, the data page in the
 * kernel's map, NULL until it is whole, the page's counts in a tick its
 * rate was made for, the rate measured, the monotonic and raw clocks at
 * the page's base in nanoseconds whole, and real time's offset over the
 * monotonic clock in nanoseconds, which may be negative. The page's fields
 * and these change under `vclock_lock`, the page's between the increments
 * of its sequence count.
 *
 * The page's count is the TSC in VCLOCK_TSC and KVM's clock in
 * nanoseconds in VCLOCK_PVCLOCK, the TSC converted through a clock page,
 * as Linux's kvm-clock counts its nanoseconds as its cycles at 1 GHz
 * (kvm_clock_read and kvm_clock_get_cycles, arch/x86/kernel/kvmclock.c:
 * 74-87, 347), so that a change KVM makes to its clock, a new rate or a
 * new system time with the stable bit kept, moves the clock as it moves
 * KVM's.
 */
static int running;
static int kvm_stopped;
static struct vdso_data *data;
static uintptr_t data_phys;
static uint32_t per_tick_used;
static uint32_t mult_measured;
static uint64_t mono_base_ns;
static uint64_t raw_base_ns;
static int64_t wall_ns;
static struct spinlock vclock_lock = SPINLOCK_INIT_NAMED("vclock_lock");

/*
 * The watch on a TSC that nothing vouches for (`timer_tsc_watch`), as
 * Linux's clocksource watchdog keeps such a TSC (kernel/time/clocksource.c:
 * 108-148, 245-294 and 419-560 in 6.12). Over each span of at least
 * WATCH_SPAN_NS of the tick's raw clock, the TSC's counts at the measured
 * rate are set against the raw clock, and a clock that parts from it by
 * more than WATCH_LIMIT_NS, Linux's margins of the TSC and the HPET
 * together, 500 parts in a million of the span, stops for good. Each span
 * ends on the next online processor after the one it began on, as Linux's
 * clocksource_watchdog moves its timer to the next online processor on
 * each pass (add_timer_on of cpumask_next), so that a processor whose TSC
 * drifts from the others' after the boot's check parts its span from the
 * clock: the vDSO reads whichever processor's TSC it runs on. The span's
 * end is read on that processor, at its own tick or at the kick the tick
 * that found the span due sends it (`vclock_watch_cpu`). A reading of the
 * tick's clock whose two TSC readings lie more than WATCH_SKEW_NS apart is
 * no measure: it is read again, up to WATCH_READS times, as Linux's
 * cs_watchdog_read reads again a read of the HPET that was held up, and
 * where every one was held up the span keeps its start and the next tick
 * tries again. A span past twice its length begins again, as Linux skips a
 * long interval. The first span, of at least WATCH_FIRST_NS, read on one
 * processor, sets the rate instead, as Linux's tsc_refine_calibration_work
 * measures the TSC again a second after boot (arch/x86/kernel/tsc.c:
 * 1314-1406): the boot's measure spans 20 ms. A first span that differs
 * from the boot's rate by more than a hundredth keeps that rate, as
 * Linux's does, and the watch goes on from there; a TSC that has not moved
 * over a span parts from the clock.
 */
#define WATCH_SPAN_NS  500000000u /* WATCHDOG_INTERVAL */
#define WATCH_FIRST_NS 1000000000u
#define WATCH_LIMIT_NS 248000u /* 4 * WATCHDOG_MAX_SKEW */
#define WATCH_SKEW_NS  124000u /* 2 * WATCHDOG_MAX_SKEW */
#define WATCH_READS    3       /* cs_watchdog_read's on 4 to 15 processors */

/*
 * The watch's state: whether the running clock reads a watched TSC, whether
 * the TSC parted from the tick's clock, whether the first span set the rate,
 * whether a span is in progress, the TSC and the raw clock where it
 * began, and for the log the span that parted or the rate set. The tick
 * sets `watch_due` once the span is long enough, and the processor
 * `watch_cpu` reads its end and clears it; each changes under
 * `vclock_lock`, and `vclock_watch_cpu` reads them first without it.
 */
static int watching;
static int watch_based;
static int watch_parted;
static int watch_refined;
static uint64_t watch_tsc;
static uint64_t watch_raw;
static uint64_t watch_ran_ns;
static uint64_t watch_span_ns;
static volatile int watch_due;
static volatile int watch_cpu;

/* The time zone settimeofday set, for a page built after it
 * (`vclock_set_tz`). */
static volatile int32_t tz_minuteswest, tz_dsttime;

/* The count of the clock's stops and starts (`vclock_generation`), raised
 * under `vclock_lock` after the page's count goes odd, so that a reader
 * that sees the new count reads the page's new mode. */
static volatile uint32_t generation;

/** `delta` counts of the TSC at `mult` / 2^`shift` nanoseconds a count, for
 * `shift` at most 32, as vdsocode.s's 128-bit product shifted gives it: the
 * product of each half, the high one's moved up by 32 - `shift`. */
static uint64_t vclock_scale(uint64_t delta, uint32_t mult, uint32_t shift)
{
    uint64_t hi = (delta >> 32) * mult, lo = (delta & 0xFFFFFFFFu) * mult;

    return (hi << (32u - shift)) + (lo >> shift);
}

/** The page's counts from its base to `cycles`; a count behind the base,
 * as another processor's TSC can be by a few counts, is none. */
static uint64_t vclock_since(uint64_t cycles)
{
    return cycles >= data->cycle_base ? cycles - data->cycle_base : 0;
}

/** The monotonic clock at the page's count `cycles`, by its base and
 * rate. */
static uint64_t vclock_at(uint64_t cycles)
{
    return mono_base_ns + vclock_scale(vclock_since(cycles), data->mult, data->shift);
}

/** The raw clock at the page's count `cycles`, by its base and rate. */
static uint64_t vclock_raw_at(uint64_t cycles)
{
    return raw_base_ns + vclock_scale(vclock_since(cycles), data->raw_mult, data->shift);
}

/** Write the page's times for the bases `mono_base_ns` and `raw_base_ns`,
 * and real time, the first plus `wall_ns`, in seconds and nanoseconds; and
 * real time's rate, the monotonic clock's. */
static void vclock_write_times(void)
{
    uint64_t real_ns = mono_base_ns + (uint64_t)wall_ns;

    data->mono_sec = (int64_t)(mono_base_ns / NSEC_PER_SEC);
    data->mono_nsec = mono_base_ns % NSEC_PER_SEC;
    data->real_sec = (int64_t)(real_ns / NSEC_PER_SEC);
    data->real_nsec = real_ns % NSEC_PER_SEC;
    data->raw_sec = (int64_t)(raw_base_ns / NSEC_PER_SEC);
    data->raw_nsec = raw_base_ns % NSEC_PER_SEC;
    data->real_mult = data->mult;
}

/** Begin and end a change of the page, under `vclock_lock`, as Linux's
 * vdso_write_begin and vdso_write_end bracket update_vsyscall
 * (include/vdso/helpers.h:31-53). The fence puts the odd count where every
 * processor sees it before a TSC reading taken after it. */
static void vclock_write_begin(void)
{
    data->seq++;
#ifndef RACH2_HOSTTEST
    __asm__ __volatile__("mfence" : : : "memory");
#else
    __asm__ __volatile__("" : : : "memory");
#endif
}

static void vclock_write_end(void)
{
    __asm__ __volatile__("" : : : "memory");
    data->seq++;
}

/* A reading of the moment now (`vclock_sample`). */
struct vclock_sample {
    uint64_t tsc; /* the running processor's TSC */
    uint64_t ns;  /* KVM's clock at it in nanoseconds, where `kvm` */
    int kvm;      /* KVM's clock was read with the TSC */
    int stable;   /* the page read had PVCLOCK_TSC_STABLE_BIT, or none is kept */
};

/** Read the moment now into `*s`: the running processor's KVM clock page
 * and its TSC in one pass, the TSC converted through the page and the
 * page's stable bit with them (`tsc_kvm_read`), as Linux's kvm_clock_read
 * reads this processor's page (kvmclock.c:74-82; pvclock.c:68-84); where
 * no page is kept, the TSC read in order, which `vclock_refusal` found one
 * clock on every processor. With interrupts off. */
static void vclock_sample(struct vclock_sample *s)
{
    struct tsc_kvm_sample k;

    s->kvm = tsc_kvm_read(&k);
    if (s->kvm) {
        s->tsc = k.tsc;
        s->ns = k.ns;
        s->stable = k.stable;
    } else {
        s->tsc = tsc_ordered();
        s->ns = 0;
        s->stable = 1;
    }
}

/** The middle of the counts or times `a` and `b`, in either order. */
static uint64_t vclock_middle(uint64_t a, uint64_t b)
{
    return a < b ? a + (b - a) / 2u : b + (a - b) / 2u;
}

/**
 * The page's count of the moment `s` read, the one clamp every count taken
 * from a reading passes before a time is computed from it or it becomes
 * the base: KVM's clock read with the TSC where a page is kept, as Linux's
 * __pvclock_clocksource_read converts the TSC through the page whether or
 * not it has the stable bit (pvclock.c:68-88), and the TSC where none is
 * kept. A count behind the page's base is the base, so that no time is
 * taken from before it and the base never moves back. The base is Linux's
 * last_value (pvclock.c:47, 86-108), which no reading without the bit goes
 * behind: every such reading the kernel gives a time from becomes the base
 * (`vclock_withdrawn`). Under `vclock_lock`.
 */
static uint64_t vclock_count(const struct vclock_sample *s)
{
    uint64_t count = s->kvm ? s->ns : s->tsc;

    return count > data->cycle_base ? count : data->cycle_base;
}

/**
 * Put the page's base at the count `cycles`, with the monotonic and raw
 * clocks there `mono` and `raw`: the only writer of the base's count and
 * the clocks' times there, which the page's times, real time's and the
 * coarse clocks' among them, follow (`vclock_write_times`). With the
 * page's count odd, under `vclock_lock`.
 */
static void vclock_anchor(uint64_t cycles, uint64_t mono, uint64_t raw)
{
    data->cycle_base = cycles;
    mono_base_ns = mono;
    raw_base_ns = raw;
}

/**
 * Advance the base to the moment now, read here, after the page's count
 * went odd, so that a reading by the old base was of a moment before it:
 * each clock goes to its time at the moment's count (`vclock_count`) at
 * its old rate, plus `mono_step` and `raw_step`, the tick's steps forward
 * (`vclock_step`), and the count becomes the base (`vclock_anchor`). The
 * one way a running clock's base moves on: the tick, a change of real
 * time's offset, the stop and a reading made with the boot processor's
 * stable bit withdrawn go through it. With the page's count odd, under
 * `vclock_lock`, with interrupts off.
 */
static void vclock_advance(uint64_t mono_step, uint64_t raw_step)
{
    struct vclock_sample s;
    uint64_t cycles;

    vclock_sample(&s);
    cycles = vclock_count(&s);
    vclock_anchor(cycles, vclock_at(cycles) + mono_step, vclock_raw_at(cycles) + raw_step);
}

/** The tick's monotonic clock now in nanoseconds, with its raw clock in
 * `*raw_ns`, read between the moments `*before` and `*after`
 * (`vclock_sample`). With interrupts off. */
static uint64_t vclock_reference(struct vclock_sample *before, struct vclock_sample *after,
                                 uint64_t *raw_ns)
{
    uint64_t mono_ns;

    vclock_sample(before);
    rtc_tick_both(&mono_ns, raw_ns);
    vclock_sample(after);
    return mono_ns;
}

/** The page's counts in a tick: a tick's nanoseconds in VCLOCK_PVCLOCK,
 * where the page counts KVM's clock, and the TSC's otherwise. */
static uint32_t vclock_counts_per_tick(void)
{
    return data->mode == VCLOCK_PVCLOCK ? TICK_NS : tsc_clock_per_tick();
}

/** The rate and its shift for `per_tick` counts a tick: the largest shift,
 * at most 32, that keeps the multiplier below 2^31, so that the steering's
 * changes of it fit 32 bits. */
static void vclock_rate(uint32_t per_tick, uint32_t *mult, uint32_t *shift)
{
    uint32_t s = 32;

    while (s > 0 && (((uint64_t)TICK_NS << s) / per_tick) >= 0x80000000u) {
        s--;
    }
    *mult = (uint32_t)(((uint64_t)TICK_NS << s) / per_tick);
    *shift = s;
}

/**
 * Build the data page, from the kernel's window, so that the kernel
 * reaches it at a fixed address, and publish it in `data` only once it is
 * whole, its count even and its mode VCLOCK_NONE, as Linux clears the clock
 * pages in kvmclock_init_mem before kvmclock_setup_percpu publishes one
 * (arch/x86/kernel/kvmclock.c:202-239, 264-285): a timer interrupt reads
 * the page through `data` (`vclock_read`), and the frame comes uncleared,
 * with a count that may be odd. x86 keeps the stores in order. 0, or -1
 * with no frame.
 */
static int vclock_page(void)
{
    struct vdso_data *page = (struct vdso_data *)alloc_kernel_frame();

    if (!page) {
        return -1;
    }
    k_memset(page, 0, PAGE_SIZE);
    page->coarse_ns = TICK_NS;
    page->getcpu = (uint32_t)tsc_has_rdtscp();
    page->tz_minuteswest = tz_minuteswest;
    page->tz_dsttime = tz_dsttime;
    data_phys = virt_to_phys(page);
    __asm__ __volatile__("" : : : "memory");
    data = page;
    return 0;
}

/* The refusal the tick tries again (`vclock_refusal`). */
static const char vclock_withdrawn_why[] = "kvm's clock page withdrew its stable bit";

/* The refusal for good of a watched TSC that parted from the tick's clock
 * (`vclock_watch`). */
static const char vclock_parted_why[] = "the tsc parted from the tick's clock";

/** Why the clock cannot run from the TSC, for the log, or NULL when it
 * can: `vclock_withdrawn_why` where only the boot processor's KVM clock
 * page, which may set its stable bit again, stands in the way. */
static const char *vclock_refusal(void)
{
    uint64_t behind;
    int cpu;

    if (watch_parted) {
        return vclock_parted_why;
    }
    if (tsc_clock_per_tick() == 0) {
        return "no tsc was measured";
    }
    if (!tsc_synced(&cpu, &behind)) {
        if (behind != 0) {
            klog(KLOG_WARN, "vdso", "tsc: cpu%d behind cpu0 by %lu counts", cpu,
                 (unsigned long)behind);
        } else {
            klog(KLOG_WARN, "vdso", "tsc: cpu%d: its check timed out", cpu);
        }
        return "the processors' tscs disagree";
    }
    if (tsc_kvm_boot_withdrawn()) {
        return vclock_withdrawn_why;
    }
    return 0;
}

/**
 * Start the clock from the tick's clocks now at the page's rate now, and
 * have the vDSO read it, in VCLOCK_PVCLOCK where KVM's clock pages are
 * kept, as Linux's kvm-clock gives the vDSO VDSO_CLOCKMODE_PVCLOCK where
 * the boot processor's page has the stable bit (kvm_setup_vsyscall_timeinfo,
 * kvmclock.c:241-262): at clock.c's floors, `*mono_ns` and `*raw_ns`,
 * where a clock stopped before is still ahead of the tick's, so that no
 * reading is earlier than one before the start. The base is a new anchor
 * at the count read with the tick's clocks, KVM's clock where its pages
 * are kept, carried from no old one, since the clock did not run. The
 * bases go back into them. Where KVM's pages are kept and either reading,
 * or the boot processor's page now, lacks the stable bit, as when KVM
 * withdraws it after the tick decided to start, the clock does not start
 * and the floors stay; the tick tries again once the bit is back. Given to
 * `rtc_vclock_change`.
 */
static void vclock_begin(uint64_t *mono_ns, uint64_t *raw_ns)
{
    struct vclock_sample before, after;
    uint64_t mono, raw, cycles;
    int kvm;

    mono = vclock_reference(&before, &after, &raw);
    kvm = before.kvm && after.kvm;
    if (kvm && (!before.stable || !after.stable || tsc_kvm_boot_withdrawn())) {
        kvm_stopped = 1;
        return;
    }
    cycles = kvm ? vclock_middle(before.ns, after.ns) : vclock_middle(before.tsc, after.tsc);
    spin_lock(&vclock_lock);
    vclock_write_begin();
    data->mode = kvm ? VCLOCK_PVCLOCK : VCLOCK_TSC;
    per_tick_used = vclock_counts_per_tick();
    vclock_rate(per_tick_used, &mult_measured, &data->shift);
    vclock_anchor(cycles, mono > *mono_ns ? mono : *mono_ns, raw > *raw_ns ? raw : *raw_ns);
    data->mult = (uint32_t)rtc_slewed(mult_measured, rtc_mono_rate());
    data->raw_mult = mult_measured;
    data->res_ns = vclock_resolution_ns();
    vclock_write_times();
    generation++;
    vclock_write_end();
    running = 1;
    kvm_stopped = 0;
    watching = !kvm && tsc_counts_per_tick() == 0;
    watch_based = 0;
    watch_due = 0;
    watch_cpu = (int)arch_cpu_id();
    *mono_ns = mono_base_ns;
    *raw_ns = raw_base_ns;
    spin_unlock(&vclock_lock);
}

/**
 * Stop the clock, so that the vDSO makes the system call and the kernel's
 * clock calls read the tick's clocks, as Linux's vDSO falls back once
 * KVM's clock page withdraws PVCLOCK_TSC_STABLE_BIT (vread_pvclock,
 * arch/x86/include/asm/vdso/gettimeofday.h:219-229). The page's count goes
 * odd first, and the clock's last times are read at a moment after it,
 * past every reading a reader has finished, the base advanced to it by
 * KVM's clock (`vclock_advance`), and go into `*mono_ns` and `*raw_ns`
 * while readers still wait, so that clock.c's
 * floors hold the tick's clocks after the stop at or past them. Given to
 * `rtc_vclock_change`.
 */
static void vclock_stop(uint64_t *mono_ns, uint64_t *raw_ns)
{
    spin_lock(&vclock_lock);
    vclock_write_begin();
    data->mode = VCLOCK_NONE;
    generation++;
    vclock_advance(0, 0);
    vclock_write_times();
    if (mono_base_ns > *mono_ns) {
        *mono_ns = mono_base_ns;
    }
    if (raw_base_ns > *raw_ns) {
        *raw_ns = raw_base_ns;
    }
    vclock_write_end();
    running = 0;
    kvm_stopped = 1;
    spin_unlock(&vclock_lock);
}

/**
 * Interrupt every other processor once the clock stopped, so that a vCPU in
 * its guest exits and its run loop, whose entries and fast path test the
 * generation (`kvm_clock_host_moved`), decides its VM's master clock again
 * before the guest reads the stable bit any longer, as Linux's
 * pvclock_gtod_notify has every vCPU of every VM asked for
 * KVM_REQ_MASTERCLOCK_UPDATE and kicked (arch/x86/kvm/x86.c:9166-9169). The
 * generation went up under `vclock_lock`; the barrier puts it before the
 * interrupts, which a write of the x2APIC's ICR would not order. Outside
 * `vclock_lock` and clock.c's locks; this processor's own vCPU, if any, is
 * outside its guest while the tick runs, and enters through the test.
 */
static void vclock_kick(void)
{
    sched_mb();
    sched_kick_cpus(~(uint64_t)0);
}

void vclock_start(void)
{
    const char *why;

    tsc_cpu_setup(0);
    if (vclock_page() != 0 || vdso_build() != 0) {
        klog(KLOG_WARN, "vdso", "absent: no memory for its pages");
        return;
    }
    why = vclock_refusal();
    if (why) {
        kvm_stopped = why == vclock_withdrawn_why;
        klog(KLOG_INFO, "vdso", "clock: the system call; %s", why);
        return;
    }
    rtc_vclock_change(vclock_begin);
    if (!running) {
        klog(KLOG_INFO, "vdso", "clock: the system call; %s", vclock_withdrawn_why);
        return;
    }
    if (watching) {
        klog(KLOG_INFO, "vdso", "clock: the tsc, %d counts a tick, watched against the %s",
             (int)tsc_clock_per_tick(), refclock_name());
        return;
    }
    klog(KLOG_INFO, "vdso", "clock: the tsc, %d counts a tick", (int)tsc_counts_per_tick());
}

int vclock_running(void)
{
    return running;
}

uint32_t vclock_generation(void)
{
    return generation;
}

/** The step forward of a clock that reads `clock_ns` where the tick's
 * reads `ref_ns`: as far as it is behind when that is more than STEP_NS,
 * else none. */
static uint64_t vclock_step(uint64_t ref_ns, uint64_t clock_ns)
{
    return clock_ns < ref_ns && ref_ns - clock_ns > STEP_NS ? ref_ns - clock_ns : 0u;
}

/** The rate for a clock that runs at `mult` and reads `clock_ns` where the
 * tick's reads `ref_ns`, steered toward the tick's; a clock stepped
 * forward (`vclock_step`) is not steered. */
static uint32_t vclock_steer(uint32_t mult, uint64_t ref_ns, uint64_t clock_ns)
{
    int ahead = clock_ns > ref_ns;
    uint32_t error, adjust;

    if (vclock_step(ref_ns, clock_ns) != 0) {
        return mult;
    }
    error = (uint32_t)(ahead ? clock_ns - ref_ns : ref_ns - clock_ns);
    if (error > SLEW_MAX_NS) {
        error = SLEW_MAX_NS;
    }
    adjust = (uint32_t)(((uint64_t)mult * error) >> SLEW_SHIFT);
    return ahead ? mult - adjust : mult + adjust;
}

/* What `vclock_watch` found: a span begun or kept, the first span's rate
 * set, the TSC parted from the tick's clock, or a span not yet long
 * enough. */
enum { WATCH_KEEP, WATCH_REFINED, WATCH_PARTED, WATCH_EARLY };

/**
 * Set the watched TSC against the tick's raw clock `ref_raw`, read between
 * the moments `*before` and `*after` on this processor, a reading not held
 * up (`vclock_watch_cpu`), as the comment at WATCH_SPAN_NS says: begin a
 * span, measure the rate over the first one, or test a later one.
 * WATCH_PARTED after it sets `watch_parted`, which the tick acts on. Under
 * `vclock_lock`.
 */
static int vclock_watch(const struct vclock_sample *before, const struct vclock_sample *after,
                        uint64_t ref_raw)
{
    uint64_t tsc, span, need, ran, delta, most, per_tick;

    tsc = vclock_middle(before->tsc, after->tsc);
    need = watch_refined ? WATCH_SPAN_NS : WATCH_FIRST_NS;
    span = ref_raw - watch_raw;
    if (!watch_based || ref_raw <= watch_raw || span > 2u * need) {
        watch_based = 1;
        watch_tsc = tsc;
        watch_raw = ref_raw;
        return WATCH_KEEP;
    }
    if (span < need) {
        return WATCH_EARLY;
    }
    /* A TSC behind the span's start ran none of it, and one past twice
     * the counts the span should hold ran twice it, which bounds the
     * products below: the span is under 2^31 ns, the rate under 2^32. */
    most = 2u * tick_div64(span * tsc_clock_per_tick(), TICK_NS, NULL);
    delta = tsc > watch_tsc ? tsc - watch_tsc : 0;
    delta = delta < most ? delta : most;
    ran = vclock_scale(delta, mult_measured, data->shift);
    watch_tsc = tsc;
    watch_raw = ref_raw;
    if (!watch_refined) {
        watch_refined = 1;
        /* Within a hundredth of the boot's measure, as Linux's refinement
         * keeps the boot's rate otherwise (arch/x86/kernel/tsc.c:1379-1381). */
        if ((ran > span ? ran - span : span - ran) > span / 100u) {
            return WATCH_KEEP;
        }
        per_tick = tick_div64(delta * TICK_NS, span, NULL);
        if (per_tick == 0 || per_tick > 0xFFFFFFFFu) {
            return WATCH_KEEP;
        }
        timer_tsc_watch_refine((uint32_t)per_tick);
        return WATCH_REFINED;
    }
    if ((ran > span ? ran - span : span - ran) > WATCH_LIMIT_NS) {
        watch_parted = 1;
        watch_ran_ns = ran;
        watch_span_ns = span;
        return WATCH_PARTED;
    }
    return WATCH_KEEP;
}

/** The online processor after `cpu`, in a ring of their indices, or `cpu`
 * where no other is online. */
static int vclock_next_cpu(int cpu)
{
    int i, c;

    for (i = 1; i <= NR_CPUS; i++) {
        c = (cpu + i) % NR_CPUS;
        if (cpu_table[c].online) {
            return c;
        }
    }
    return cpu;
}

/**
 * Whether the watch's span is due to end, by the tick's raw clock
 * `ref_raw`: one not yet begun, or as long as it must be by a reading
 * later than its start, which a processor that read the start after the
 * tick read `ref_raw` may have put past it. A due span whose
 * processor is no longer online passes to the next. The processor to kick
 * so that it reads the span's end, or -1 where that is this one, which
 * reads it at this tick. Under `vclock_lock`.
 */
static int vclock_watch_due(uint64_t ref_raw)
{
    uint64_t need = watch_refined ? WATCH_SPAN_NS : WATCH_FIRST_NS;

    if (!watch_due && (!watch_based || (ref_raw > watch_raw && ref_raw - watch_raw >= need))) {
        watch_due = 1;
    }
    if (!watch_due) {
        return -1;
    }
    if (!cpu_table[watch_cpu].online) {
        watch_cpu = vclock_next_cpu(watch_cpu);
    }
    return watch_cpu != (int)arch_cpu_id() ? watch_cpu : -1;
}

/** The tick's clock is read outside `vclock_lock`, up to WATCH_READS
 * times, until one reading's two TSC readings lie within WATCH_SKEW_NS at
 * the watched rate; the span is judged under the lock, where it is still
 * this processor's and due. */
void vclock_watch_cpu(void)
{
    struct vclock_sample before, after;
    uint64_t ref_raw = 0, skew;
    int me = (int)arch_cpu_id();
    int i, found = WATCH_KEEP, refined;

    if (!watch_due || watch_cpu != me) {
        return;
    }
    skew = tick_div64((uint64_t)WATCH_SKEW_NS * tsc_clock_per_tick(), TICK_NS, NULL);
    for (i = 0; i < WATCH_READS; i++) {
        (void)vclock_reference(&before, &after, &ref_raw);
        if (after.tsc >= before.tsc && after.tsc - before.tsc <= skew) {
            break;
        }
    }
    if (i == WATCH_READS) {
        return;
    }
    spin_lock(&vclock_lock);
    if (running && watching && !watch_parted && watch_due && watch_cpu == me) {
        refined = watch_refined;
        found = vclock_watch(&before, &after, ref_raw);
        if (found != WATCH_EARLY) {
            watch_due = 0;
            if (refined) {
                watch_cpu = vclock_next_cpu(me);
            }
        }
    }
    spin_unlock(&vclock_lock);
    if (found == WATCH_REFINED) {
        klog(KLOG_INFO, "vdso", "clock: tsc: refined to %d counts a tick",
             (int)tsc_clock_per_tick());
    }
}

/**
 * The errors are measured at the tick's reading, its counts taken as every
 * count is (`vclock_count`), and the base advanced to a moment read after
 * the page's count went odd (`vclock_advance`), so that a reading that
 * used the old rate was of a count before the new base, and each clock is
 * continuous there: the base's time is the old rate's time at that count.
 * Where KVM's clock pages are kept the counts are KVM's clock, so that a
 * new rate or system time KVM gives its pages moves the clock as it moves
 * KVM's, and a stable bit KVM withdraws moves no clock by a TSC that may
 * not agree with the base's. The monotonic clock's rate is the measured
 * one at the tick length's rate, under 2^32 since the measured is under
 * 2^31 and the length within a quarter of a tick. Real time goes with the
 * monotonic clock, a step forward of it included.
 *
 * The boot processor's clock page, the one every reader tests
 * (`tsc_kvm_boot_withdrawn`), decides, as Linux's vDSO reads only that page
 * while the bit holds (arch/x86/include/asm/vdso/gettimeofday.h:219): a
 * withdrawn stable bit there stops the clock, or keeps it from starting at
 * boot, and the bit set again starts it afresh from the tick's clocks, or
 * from the last time a reading gave where that is later
 * (`rtc_vclock_change`).
 *
 * A watched TSC's span that is due to end has its processor kicked, so
 * that it reads the end even with its tick stopped in an idle wait, as
 * Linux's add_timer_on wakes the processor it puts the watchdog's timer
 * on (`vclock_watch_due`, `vclock_watch_cpu`); a span that parted stops
 * the clock at the next tick.
 */
void vclock_tick(void)
{
    struct vclock_sample before, after;
    uint64_t at_ref, ref_ns, ref_raw, clock_ns, clock_raw;
    uint32_t mono_mult;
    int kick = -1;

    if (!running) {
        if (kvm_stopped && !vclock_refusal()) {
            rtc_vclock_change(vclock_begin);
            if (running) {
                klog(KLOG_INFO, "vdso", "clock: the tsc again, %d counts a tick",
                     (int)tsc_counts_per_tick());
            }
        }
        return;
    }
    if (watch_parted) {
        rtc_vclock_change(vclock_stop);
        vclock_kick();
        klog(KLOG_WARN, "vdso", "clock: the system call; the tsc ran %lu ns in %lu ns of the %s",
             (unsigned long)watch_ran_ns, (unsigned long)watch_span_ns, refclock_name());
        return;
    }
    if (tsc_kvm_boot_withdrawn()) {
        rtc_vclock_change(vclock_stop);
        vclock_kick();
        klog(KLOG_WARN, "vdso", "clock: the system call; %s", vclock_withdrawn_why);
        return;
    }
    ref_ns = vclock_reference(&before, &after, &ref_raw);
    spin_lock(&vclock_lock);
    at_ref = vclock_middle(vclock_count(&before), vclock_count(&after));
    clock_ns = vclock_at(at_ref);
    clock_raw = vclock_raw_at(at_ref);
    if (watching) {
        kick = vclock_watch_due(ref_raw);
    }
    vclock_write_begin();
    vclock_advance(vclock_step(ref_ns, clock_ns), vclock_step(ref_raw, clock_raw));
    if (vclock_counts_per_tick() != per_tick_used) {
        per_tick_used = vclock_counts_per_tick();
        vclock_rate(per_tick_used, &mult_measured, &data->shift);
    }
    data->res_ns = vclock_resolution_ns();
    mono_mult = (uint32_t)rtc_slewed(mult_measured, rtc_mono_rate());
    data->mult = vclock_steer(mono_mult, ref_ns, clock_ns);
    data->raw_mult = vclock_steer(mult_measured, ref_raw, clock_raw);
    vclock_write_times();
    vclock_write_end();
    spin_unlock(&vclock_lock);
    if (kick >= 0) {
        sched_kick_cpus((uint64_t)1 << kick);
    }
}

/** The page's base seconds, nanoseconds and rate of the clock `which`. */
static void vclock_fields(int which, int64_t *sec, uint64_t *nsec, uint32_t *mult)
{
    if (which == VCLOCK_READ_REAL) {
        *sec = data->real_sec;
        *nsec = data->real_nsec;
        *mult = data->real_mult;
    } else if (which == VCLOCK_READ_RAW) {
        *sec = data->raw_sec;
        *nsec = data->raw_nsec;
        *mult = data->raw_mult;
    } else {
        *sec = data->mono_sec;
        *nsec = data->mono_nsec;
        *mult = data->mult;
    }
}

/* What a reading's passes found (`vclock_pass`). */
#define VREAD_OK        0 /* read as the vDSO reads it */
#define VREAD_STOPPED   1 /* the clock does not run; nothing read */
#define VREAD_WITHDRAWN 2 /* KVM's page withdrew its bit; nothing read */

/**
 * A reading of the clock `which` as the vDSO makes it: Linux's do_hres,
 * and do_coarse with `coarse`, each pass checking the mode as do_hres does
 * (lib/vdso/gettimeofday.c:130-179, 218-242), and in VCLOCK_PVCLOCK the
 * count read as vread_pvclock reads it, the TSC converted through KVM's
 * page for the boot processor while that page has its stable bit
 * (`tsc_kvm_boot_read`; arch/x86/include/asm/vdso/gettimeofday.h:
 * 197-232). The base's seconds go into `*sec`, the nanoseconds since them,
 * which may pass a second, into `*ns`, and the TSC the count was read at
 * into `*cycles`, which a coarse reading leaves. VREAD_OK; VREAD_STOPPED
 * while the clock does not run, and VREAD_WITHDRAWN where the page has
 * withdrawn its bit, where the vDSO makes the system call, each with
 * nothing that may be used.
 */
static int vclock_pass(int which, int coarse, int64_t *sec, uint64_t *ns, uint64_t *cycles)
{
    uint32_t seq, mult, mode;
    uint64_t count;

    do {
        while ((seq = data->seq) & 1u) {
            arch_relax();
        }
        __asm__ __volatile__("" : : : "memory");
        mode = data->mode;
        if (mode == VCLOCK_NONE) {
            return VREAD_STOPPED;
        }
        vclock_fields(which, sec, ns, &mult);
        if (!coarse) {
            if (mode != VCLOCK_PVCLOCK) {
                *cycles = tsc_ordered();
                count = *cycles;
            } else if (!tsc_kvm_boot_read(cycles, &count)) {
                return VREAD_WITHDRAWN;
            }
            *ns += vclock_scale(vclock_since(count), mult, data->shift);
        }
        __asm__ __volatile__("" : : : "memory");
    } while (data->seq != seq);
    return VREAD_OK;
}

/**
 * A reading of the clock `which` made where `vclock_pass` found the boot
 * processor's page without its stable bit, the reading the vDSO's system
 * call makes: the base advanced to KVM's clock now, read from the running
 * processor's page (`vclock_advance`), as the stop advances it and as
 * Linux's kernel goes on reading kvm-clock (kvmclock.c:74-82), and the
 * base's time given, in `*sec` and in `*ns`, under a second. Since the
 * count of every time given this way is the base, a later reading without
 * the bit is held to it (`vclock_count`), and so is a reader of the boot
 * processor's page once the bit is back, the vDSO's and `vclock_pass`,
 * which hold their counts to the base, before any other writer moves it.
 * VREAD_OK, or VREAD_STOPPED where the clock no longer runs from KVM's
 * pages or the running processor keeps none. Takes `vclock_lock`.
 */
static int vclock_withdrawn(int which, int64_t *sec, uint64_t *ns)
{
    struct vclock_sample s;
    uint32_t flags = spin_lock_irqsave(&vclock_lock);
    uint32_t mult;
    int found = VREAD_STOPPED;

    vclock_sample(&s);
    if (data->mode == VCLOCK_PVCLOCK && s.kvm) {
        vclock_write_begin();
        vclock_advance(0, 0);
        vclock_write_times();
        vclock_write_end();
        vclock_fields(which, sec, ns, &mult);
        found = VREAD_OK;
    }
    spin_unlock_irqrestore(&vclock_lock, flags);
    return found;
}

/** A reading of `vclock_pass`, or of `vclock_withdrawn` where the boot
 * processor's page withdrew its bit, so that a system call the vDSO makes
 * for a withdrawn bit reads the clock on from where its readings left
 * it. */
int vclock_read(int which, int coarse, int64_t *sec, int64_t *nsec)
{
    uint64_t ns, cycles;
    int64_t s;
    int found;

    if (!data) {
        return 0;
    }
    found = vclock_pass(which, coarse, &s, &ns, &cycles);
    if (found == VREAD_WITHDRAWN) {
        found = vclock_withdrawn(which, &s, &ns);
    }
    if (found != VREAD_OK) {
        return 0;
    }
    if (ns >= NSEC_PER_SEC) {
        s += (int64_t)(ns / NSEC_PER_SEC);
        ns %= NSEC_PER_SEC;
    }
    *sec = s;
    *nsec = (int64_t)ns;
    return 1;
}

/** Only a reading VREAD_OK pairs the TSC it was read at with the time,
 * which in VCLOCK_PVCLOCK is taken from the count converted from that TSC,
 * as Linux's vgettsc pairs the TSC with the count a valid Hyper-V page
 * converts it to (arch/x86/kvm/x86.c:2402-2413); a withdrawn bit gives no
 * pair, as vgettsc gives KVM none from a clock page that is not valid
 * (x86.c:2409-2425). */
int vclock_read_cycles(int which, uint64_t *ns, uint64_t *cycles)
{
    uint64_t c, nsec;
    int64_t sec;

    if (!data || vclock_pass(which, 0, &sec, &nsec, &c) != VREAD_OK) {
        return 0;
    }
    *ns = (uint64_t)sec * NSEC_PER_SEC + nsec;
    *cycles = c;
    return 1;
}

/** The bases go on to the moment of the change (`vclock_advance`), by
 * KVM's clock where its pages are kept, so that a change made once KVM has
 * withdrawn its stable bit moves no clock by a TSC that may not agree with
 * the base's. */
void vclock_set_wall(int64_t sec, int32_t nsec)
{
    uint32_t flags = spin_lock_irqsave(&vclock_lock);

    if (running) {
        vclock_write_begin();
        vclock_advance(0, 0);
    }
    wall_ns = sec * (int64_t)NSEC_PER_SEC + nsec;
    if (running) {
        vclock_write_times();
        vclock_write_end();
    }
    spin_unlock_irqrestore(&vclock_lock, flags);
}

void vclock_set_tz(int32_t minuteswest, int32_t dsttime)
{
    struct vdso_data *page = data;

    tz_minuteswest = minuteswest;
    tz_dsttime = dsttime;
    if (page) {
        page->tz_minuteswest = minuteswest;
        page->tz_dsttime = dsttime;
    }
}

uint32_t vclock_resolution_ns(void)
{
    uint32_t per_tick = tsc_clock_per_tick();

    return per_tick ? (TICK_NS + per_tick - 1u) / per_tick : TICK_NS;
}

uintptr_t vdso_data_frame(void)
{
    return data_phys;
}
