/*
 * vclock.c: the clock the x86-64's vDSO reads, kept from the TSC in the
 * data page beside the vDSO's image, and read by the kernel's own clock
 * calls as well while it runs (../../vdso.h).
 */

#include "../../vdso.h"
#include "vdsoimg.h"

#include "../arch.h"
#include "../x86/pit.h"
#include "../x86/tsc.h"

#include "../../klog.h"
#include "../../rtc.h"
#include "../../timer.h"
#include "../../lib/kirq.h"
#include "../../lib/kstring.h"
#include "../../lib/spinlock.h"
#include "../../mm/paging.h"

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
                    __builtin_offsetof(struct vdso_data, raw_nsec) == VDSO_DATA_RAW_NSEC)
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
 * The clock: whether it runs, and whether it stopped because KVM's clock
 * page withdrew its stable bit, the data page in the kernel's map, NULL
 * until it is whole, the TSC's counts in a tick the page's rate was made
 * for, the rate measured, the monotonic and raw clocks at the page's base
 * in nanoseconds whole, real time's offset over the monotonic clock in
 * nanoseconds, which may be negative, and KVM's clock at the page's base
 * where it was read with the base's count (`kvm_base_read`). The page's
 * fields and these change under `vclock_lock`, the page's between the
 * increments of its sequence count.
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
static uint64_t kvm_base_ns;
static int kvm_base_read;
static struct spinlock vclock_lock = SPINLOCK_INIT_NAMED("vclock_lock");

/** `delta` counts of the TSC at `mult` / 2^`shift` nanoseconds a count, for
 * `shift` at most 32, as vdsocode.s's 128-bit product shifted gives it: the
 * product of each half, the high one's moved up by 32 - `shift`. */
static uint64_t vclock_scale(uint64_t delta, uint32_t mult, uint32_t shift)
{
    uint64_t hi = (delta >> 32) * mult, lo = (delta & 0xFFFFFFFFu) * mult;

    return (hi << (32u - shift)) + (lo >> shift);
}

/** The counts of the TSC from the page's base to `cycles`; a count behind
 * the base, as another processor's can be by a few counts, is none. */
static uint64_t vclock_since(uint64_t cycles)
{
    return cycles >= data->cycle_base ? cycles - data->cycle_base : 0;
}

/** The monotonic clock at the TSC's count `cycles`, by the page's base and
 * rate. */
static uint64_t vclock_at(uint64_t cycles)
{
    return mono_base_ns + vclock_scale(vclock_since(cycles), data->mult, data->shift);
}

/** The raw clock at the TSC's count `cycles`, by the page's base and
 * rate. */
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

/** The tick's monotonic clock now in nanoseconds, with its raw clock in
 * `*raw_ns`, and the TSC at that reading, the middle of a reading on each
 * side; where KVM's clock pages are kept, each side reads the TSC with
 * KVM's clock from the running processor's page (`tsc_kvm_now`), and KVM's
 * clock at the middle goes into `*kvm_ns` with `*kvm` 1, else `*kvm` is 0.
 * With interrupts off. */
static uint64_t vclock_reference(uint64_t *cycles, uint64_t *raw_ns, uint64_t *kvm_ns, int *kvm)
{
    uint64_t t0, n0, t1, n1, mono_ns;

    *kvm = tsc_kvm_now(&t0, &n0);
    if (!*kvm) {
        t0 = tsc_ordered();
    }
    rtc_tick_both(&mono_ns, raw_ns);
    if (*kvm && tsc_kvm_now(&t1, &n1)) {
        *kvm_ns = n0 + (n1 - n0) / 2u;
    } else {
        *kvm = 0;
        t1 = tsc_ordered();
    }
    *cycles = t0 + (t1 - t0) / 2u;
    return mono_ns;
}

/** The TSC now, for the page's new base, read with KVM's clock at it from
 * the running processor's page where KVM's pages are kept, into
 * `kvm_base_ns`, as Linux's __pvclock_clocksource_read reads the two in
 * one pass (arch/x86/kernel/pvclock.c:75-79); else read in order. Under
 * `vclock_lock`, with interrupts off. */
static uint64_t vclock_base_cycles(void)
{
    uint64_t cycles;

    kvm_base_read = tsc_kvm_now(&cycles, &kvm_base_ns);
    return kvm_base_read ? cycles : tsc_ordered();
}

/**
 * The count on the page's TSC of the moment now, for the stop: where KVM's
 * clock was read at the page's base, the base plus KVM's clock since then,
 * read from the running processor's own page, at the page's counts in a
 * tick. KVM's clock is one clock on every processor; the TSC of the
 * processor that stops the clock, once the stable bit is withdrawn, may not
 * agree with the one the base was read on. Linux's reader likewise
 * converts each processor's own count by its own page before it takes any
 * floor (__pvclock_clocksource_read, pvclock.c:75-79 and 104-110;
 * kvm_clock_read, kvmclock.c:74-82). Elsewhere, the TSC read in order.
 * Under `vclock_lock`, with interrupts off.
 */
static uint64_t vclock_stop_cycles(void)
{
    uint64_t tsc, ns, gone;

    if (!kvm_base_read || !tsc_kvm_now(&tsc, &ns)) {
        return tsc_ordered();
    }
    gone = ns > kvm_base_ns ? ns - kvm_base_ns : 0u;
    return data->cycle_base + gone / TICK_NS * per_tick_used +
           gone % TICK_NS * per_tick_used / TICK_NS;
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
    data_phys = virt_to_phys(page);
    __asm__ __volatile__("" : : : "memory");
    data = page;
    return 0;
}

/** Why the clock cannot run from the TSC, for the log, or NULL when it
 * can. */
static const char *vclock_refusal(void)
{
    uint64_t behind;
    int cpu;

    if (tsc_counts_per_tick() == 0) {
        return "no tsc at one rate was measured";
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
    if (tsc_kvm_withdrawn()) {
        return "kvm's clock page withdrew its stable bit";
    }
    return 0;
}

/**
 * Start the clock from the tick's clocks now at the TSC's rate now, and
 * have the vDSO read it: at clock.c's floors, `*mono_ns` and `*raw_ns`,
 * where a clock stopped before is still ahead of the tick's, so that no
 * reading is earlier than one before the start. The bases go back into
 * them. Given to `rtc_vclock_change`.
 */
static void vclock_begin(uint64_t *mono_ns, uint64_t *raw_ns)
{
    uint64_t cycles, mono, raw, kvm_ns = 0;
    int kvm;

    mono = vclock_reference(&cycles, &raw, &kvm_ns, &kvm);
    spin_lock(&vclock_lock);
    vclock_write_begin();
    kvm_base_read = kvm;
    kvm_base_ns = kvm_ns;
    per_tick_used = tsc_counts_per_tick();
    vclock_rate(per_tick_used, &mult_measured, &data->shift);
    mono_base_ns = mono > *mono_ns ? mono : *mono_ns;
    raw_base_ns = raw > *raw_ns ? raw : *raw_ns;
    data->cycle_base = cycles;
    data->mult = (uint32_t)rtc_slewed(mult_measured, rtc_mono_rate());
    data->raw_mult = mult_measured;
    data->res_ns = vclock_resolution_ns();
    vclock_write_times();
    data->mode = VCLOCK_TSC;
    vclock_write_end();
    running = 1;
    kvm_stopped = 0;
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
 * past every reading a reader has finished, carried from the page's base by
 * KVM's clock where it is read (`vclock_stop_cycles`), and go into
 * `*mono_ns` and `*raw_ns` while readers still wait, so that clock.c's
 * floors hold the tick's clocks after the stop at or past them. Given to
 * `rtc_vclock_change`.
 */
static void vclock_stop(uint64_t *mono_ns, uint64_t *raw_ns)
{
    uint64_t cycles, mono, raw;

    spin_lock(&vclock_lock);
    vclock_write_begin();
    data->mode = VCLOCK_NONE;
    cycles = vclock_stop_cycles();
    mono = vclock_at(cycles);
    raw = vclock_raw_at(cycles);
    if (mono > *mono_ns) {
        *mono_ns = mono;
    }
    if (raw > *raw_ns) {
        *raw_ns = raw;
    }
    vclock_write_end();
    running = 0;
    kvm_stopped = 1;
    spin_unlock(&vclock_lock);
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
        klog(KLOG_INFO, "vdso", "clock: the system call; %s", why);
        return;
    }
    rtc_vclock_change(vclock_begin);
    klog(KLOG_INFO, "vdso", "clock: the tsc, %d counts a tick", (int)tsc_counts_per_tick());
}

int vclock_running(void)
{
    return running;
}

/**
 * The rate for a clock that runs at `mult` and reads `clock_ns` where the
 * tick's reads `ref_ns`, steered toward the tick's; a clock more than
 * STEP_NS behind has its base `*base_ns` moved forward as far and is not
 * steered. The base is the clock at the page's new base, after the
 * reading.
 */
static uint32_t vclock_steer(uint32_t mult, uint64_t ref_ns, uint64_t clock_ns, uint64_t *base_ns)
{
    int ahead = clock_ns > ref_ns;
    uint32_t error, adjust;

    if (!ahead && ref_ns - clock_ns > STEP_NS) {
        *base_ns += ref_ns - clock_ns;
        return mult;
    }
    error = (uint32_t)(ahead ? clock_ns - ref_ns : ref_ns - clock_ns);
    if (error > SLEW_MAX_NS) {
        error = SLEW_MAX_NS;
    }
    adjust = (uint32_t)(((uint64_t)mult * error) >> SLEW_SHIFT);
    return ahead ? mult - adjust : mult + adjust;
}

/**
 * The errors are measured at the tick's reading and the base moved to a
 * TSC count taken after the page's count went odd, so that a reading that
 * used the old rate was of a count before the new base, and each clock is
 * continuous there: the base's time is the old rate's time at that count.
 * The monotonic clock's rate is the measured one at the tick length's
 * rate, under 2^32 since the measured is under 2^31 and the length within a
 * quarter of a tick. Real time goes with the monotonic clock, a step
 * forward of it included.
 *
 * KVM's clock page, which the tick has just read (`tsc_kvm_sample`,
 * pit.c), is followed as Linux's reader takes it at every reading
 * (arch/x86/kernel/pvclock.c:75-88): a new rate is the measured rate from
 * the new base on, a withdrawn stable bit stops the clock, and the bit set
 * again starts it afresh from the tick's clocks, or from the last time a
 * reading gave where that is later (`rtc_vclock_change`).
 */
void vclock_tick(void)
{
    uint64_t at_ref, cycles, ref_ns, ref_raw, clock_ns, clock_raw, kvm_ns;
    uint32_t mono_mult;
    int kvm;

    if (!running) {
        if (kvm_stopped && !vclock_refusal()) {
            rtc_vclock_change(vclock_begin);
            klog(KLOG_INFO, "vdso", "clock: the tsc again, %d counts a tick", (int)per_tick_used);
        }
        return;
    }
    if (tsc_kvm_withdrawn()) {
        rtc_vclock_change(vclock_stop);
        klog(KLOG_WARN, "vdso", "clock: the system call; kvm's clock page withdrew its stable bit");
        return;
    }
    ref_ns = vclock_reference(&at_ref, &ref_raw, &kvm_ns, &kvm);
    spin_lock(&vclock_lock);
    clock_ns = vclock_at(at_ref);
    clock_raw = vclock_raw_at(at_ref);
    vclock_write_begin();
    cycles = vclock_base_cycles();
    mono_base_ns = vclock_at(cycles);
    raw_base_ns = vclock_raw_at(cycles);
    if (tsc_counts_per_tick() != per_tick_used) {
        per_tick_used = tsc_counts_per_tick();
        vclock_rate(per_tick_used, &mult_measured, &data->shift);
        data->res_ns = vclock_resolution_ns();
    }
    mono_mult = (uint32_t)rtc_slewed(mult_measured, rtc_mono_rate());
    data->mult = vclock_steer(mono_mult, ref_ns, clock_ns, &mono_base_ns);
    data->raw_mult = vclock_steer(mult_measured, ref_raw, clock_raw, &raw_base_ns);
    data->cycle_base = cycles;
    vclock_write_times();
    vclock_write_end();
    spin_unlock(&vclock_lock);
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

/** Linux's do_hres, and do_coarse with `coarse`, each pass checking the
 * mode as do_hres does (lib/vdso/gettimeofday.c:130-179, 218-242). */
int vclock_read(int which, int coarse, int64_t *sec, int64_t *nsec)
{
    uint64_t ns, cycles;
    uint32_t seq, mult;
    int64_t s;

    if (!data) {
        return 0;
    }
    do {
        while ((seq = data->seq) & 1u) {
            arch_relax();
        }
        __asm__ __volatile__("" : : : "memory");
        if (data->mode != VCLOCK_TSC) {
            return 0;
        }
        vclock_fields(which, &s, &ns, &mult);
        if (!coarse) {
            cycles = tsc_ordered();
            ns += vclock_scale(vclock_since(cycles), mult, data->shift);
        }
        __asm__ __volatile__("" : : : "memory");
    } while (data->seq != seq);
    if (ns >= NSEC_PER_SEC) {
        s += (int64_t)(ns / NSEC_PER_SEC);
        ns %= NSEC_PER_SEC;
    }
    *sec = s;
    *nsec = (int64_t)ns;
    return 1;
}

int vclock_read_cycles(int which, uint64_t *ns, uint64_t *cycles)
{
    uint64_t base, c, nsec;
    uint32_t seq, mult;
    int64_t sec;

    if (!data) {
        return 0;
    }
    do {
        while ((seq = data->seq) & 1u) {
            arch_relax();
        }
        __asm__ __volatile__("" : : : "memory");
        if (data->mode != VCLOCK_TSC) {
            return 0;
        }
        vclock_fields(which, &sec, &nsec, &mult);
        base = (uint64_t)sec * NSEC_PER_SEC + nsec;
        c = tsc_ordered();
        base += vclock_scale(vclock_since(c), mult, data->shift);
        __asm__ __volatile__("" : : : "memory");
    } while (data->seq != seq);
    *ns = base;
    *cycles = c;
    return 1;
}

void vclock_set_wall(int64_t sec, int32_t nsec)
{
    uint64_t cycles;
    uint32_t flags = spin_lock_irqsave(&vclock_lock);

    if (running) {
        vclock_write_begin();
        cycles = vclock_base_cycles();
        mono_base_ns = vclock_at(cycles);
        raw_base_ns = vclock_raw_at(cycles);
        data->cycle_base = cycles;
    }
    wall_ns = sec * (int64_t)NSEC_PER_SEC + nsec;
    if (running) {
        vclock_write_times();
        vclock_write_end();
    }
    spin_unlock_irqrestore(&vclock_lock, flags);
}

uint32_t vclock_resolution_ns(void)
{
    uint32_t per_tick = tsc_counts_per_tick();

    return per_tick ? (TICK_NS + per_tick - 1u) / per_tick : TICK_NS;
}

uintptr_t vdso_data_frame(void)
{
    return data_phys;
}
