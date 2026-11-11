/*
 * vclock.c: the clock the x86-64's vDSO reads, kept from the TSC in the
 * data page beside the vDSO's image, and read by the kernel's own clock
 * calls as well while it runs (../../vdso.h).
 */

#include "../../vdso.h"
#include "vdsoimg.h"

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
                    __builtin_offsetof(struct vdso_data, coarse_ns) == VDSO_DATA_COARSE_NS)
                       ? 1
                       : -1];
#endif

#define NSEC_PER_SEC 1000000000u

/*
 * The steering. Each tick the clock's error against the reference, the
 * reference less the clock, is to be made good over the next 2^SLEW_SHIFT
 * nanoseconds, about 16.8 ms, by a rate that much faster or slower than
 * the measured one; the error is cut to SLEW_MAX_NS, so the rate is never
 * more than SLEW_MAX_NS / 2^SLEW_SHIFT, one part in 2000, from the measured
 * rate. A clock more than STEP_NS behind, as after a stretch the boot
 * processor spent with interrupts off longer than the steering can make
 * good, is stepped forward; one ahead is only slowed, since a step back
 * would make a reading earlier than one before it.
 */
#define SLEW_SHIFT  24
#define SLEW_MAX_NS 8388
#define STEP_NS     1000000u

/*
 * The clock: whether it runs, the data page in the kernel's map, the rate
 * measured, the monotonic clock and real time at the page's base in
 * nanoseconds whole, and the course real time follows (rtc.h). The page's
 * fields and these change under `vclock_lock`, the page's between the
 * increments of its sequence count.
 */
static int running;
static struct vdso_data *data;
static uintptr_t data_phys;
static uint32_t mult_measured;
static uint64_t mono_base_ns;
static uint64_t real_base_ns;
static struct rtc_course course;
static struct spinlock vclock_lock = SPINLOCK_INIT_NAMED("vclock_lock");

/** `delta` counts of the TSC at `mult` / 2^`shift` nanoseconds a count, for
 * `shift` at most 32, as vdsocode.s's 128-bit product shifted gives it: the
 * product of each half, the high one's moved up by 32 - `shift`. */
static uint64_t vclock_scale(uint64_t delta, uint32_t mult, uint32_t shift)
{
    uint64_t hi = (delta >> 32) * mult, lo = (delta & 0xFFFFFFFFu) * mult;

    return (hi << (32u - shift)) + (lo >> shift);
}

/** The monotonic clock at the TSC's count `cycles`, by the page's base and
 * rate; a count behind the base reads as the base. */
static uint64_t vclock_at(uint64_t cycles)
{
    uint64_t delta = cycles >= data->cycle_base ? cycles - data->cycle_base : 0;

    return mono_base_ns + vclock_scale(delta, data->mult, data->shift);
}

/** Real time at the TSC's count `cycles`, by the page's base and real
 * time's rate; a count behind the base reads as the base. */
static uint64_t vclock_real_at(uint64_t cycles)
{
    uint64_t delta = cycles >= data->cycle_base ? cycles - data->cycle_base : 0;

    return real_base_ns + vclock_scale(delta, data->real_mult, data->shift);
}

/** Real time that the course gives at the monotonic time `mono_ns`. */
static uint64_t vclock_course_ns(uint64_t mono_ns)
{
    int64_t off_sec;
    int32_t off_nsec;
    uint64_t frac;

    rtc_course_at(&course, (int64_t)(mono_ns / NSEC_PER_SEC), (int64_t)(mono_ns % NSEC_PER_SEC),
                  &off_sec, &off_nsec, &frac);
    return mono_ns + (uint64_t)(off_sec * NSEC_PER_SEC + off_nsec);
}

/** Real time's rate for the monotonic clock's `mult`: that times the
 * course's, plus the steering of `error`, the course less the page's real
 * time at the base, made good over 2^SLEW_SHIFT nanoseconds and cut to
 * SLEW_MAX_NS, as the monotonic clock's own. Under 2^32: `mult` is under
 * 2^31 and the course's rate under a quarter. */
static uint32_t vclock_real_rate(uint32_t mult, int64_t error)
{
    int64_t r32 = course.rate / ((int64_t)1 << (RTC_RATE_SHIFT - 32));
    uint64_t part = ((uint64_t)mult * (uint64_t)(r32 < 0 ? -r32 : r32)) >> 32;
    uint64_t rate = r32 < 0 ? mult - part : mult + part;
    uint64_t steer;

    if (error > SLEW_MAX_NS) {
        error = SLEW_MAX_NS;
    } else if (error < -SLEW_MAX_NS) {
        error = -SLEW_MAX_NS;
    }
    steer = ((uint64_t)mult * (uint64_t)(error < 0 ? -error : error)) >> SLEW_SHIFT;
    return (uint32_t)(error < 0 ? rate - steer : rate + steer);
}

/** Write the page's times for the bases `mono_base_ns` and `real_base_ns`,
 * in seconds and nanoseconds. */
static void vclock_write_times(void)
{
    data->mono_sec = (int64_t)(mono_base_ns / NSEC_PER_SEC);
    data->mono_nsec = mono_base_ns % NSEC_PER_SEC;
    data->real_sec = (int64_t)(real_base_ns / NSEC_PER_SEC);
    data->real_nsec = real_base_ns % NSEC_PER_SEC;
}

/** Begin and end a change of the page, under `vclock_lock`. The fence
 * puts the odd count where every processor sees it before a TSC reading
 * taken after it. */
static void vclock_write_begin(void)
{
    data->seq++;
    __asm__ __volatile__("mfence" : : : "memory");
}

static void vclock_write_end(void)
{
    __asm__ __volatile__("" : : : "memory");
    data->seq++;
}

/** The reference's monotonic clock now in nanoseconds, the clock the tick
 * keeps, and the TSC at that reading, the middle of a reading on each
 * side. */
static uint64_t vclock_reference(uint64_t *cycles)
{
    uint64_t ticks, before;
    uint32_t phase_ns;

    before = tsc_ordered();
    rtc_tick_phase(&ticks, &phase_ns);
    *cycles = before + (tsc_ordered() - before) / 2u;
    return ticks * TICK_NS + phase_ns;
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

/** Build the data page, from the kernel's window, so that the kernel
 * reaches it at a fixed address; 0, or -1 with no frame. */
static int vclock_page(void)
{
    data = (struct vdso_data *)alloc_kernel_frame();
    if (!data) {
        return -1;
    }
    k_memset(data, 0, PAGE_SIZE);
    data_phys = virt_to_phys(data);
    data->coarse_ns = TICK_NS;
    data->getcpu = (uint32_t)tsc_has_rdtscp();
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
    return 0;
}

void vclock_start(void)
{
    const char *why;
    uint64_t cycles;
    uint32_t flags;

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
    rtc_get_course(&course);
    flags = spin_lock_irqsave(&vclock_lock);
    vclock_rate(tsc_counts_per_tick(), &mult_measured, &data->shift);
    vclock_write_begin();
    mono_base_ns = vclock_reference(&cycles);
    real_base_ns = vclock_course_ns(mono_base_ns);
    data->cycle_base = cycles;
    data->mult = mult_measured;
    data->real_mult = vclock_real_rate(mult_measured, 0);
    data->res_ns = vclock_resolution_ns();
    vclock_write_times();
    data->mode = VCLOCK_TSC;
    vclock_write_end();
    running = 1;
    spin_unlock_irqrestore(&vclock_lock, flags);
    klog(KLOG_INFO, "vdso", "clock: the tsc, %d counts a tick", (int)tsc_counts_per_tick());
}

int vclock_running(void)
{
    return running;
}

/**
 * The error is measured at the reference's reading and the base moved to a
 * TSC count taken after the page's count went odd, so that a reading that
 * used the old rate was of a count before the new base, and the clock is
 * continuous there: the base's time is the old rate's time at that count.
 * Real time is carried on the same way, by its own rate, and steered toward
 * the course at the monotonic clock's new base; a step of the monotonic
 * clock forward moves it as far, and so does one of more than STEP_NS
 * behind the course.
 */
void vclock_tick(void)
{
    uint64_t at_ref, cycles, ref_ns, clock_ns, target;
    uint32_t error, adjust;
    int ahead;

    if (!running) {
        return;
    }
    ref_ns = vclock_reference(&at_ref);
    spin_lock(&vclock_lock);
    clock_ns = vclock_at(at_ref);
    ahead = clock_ns > ref_ns;
    vclock_write_begin();
    cycles = tsc_ordered();
    mono_base_ns = vclock_at(cycles);
    real_base_ns = vclock_real_at(cycles);
    if (!ahead && ref_ns - clock_ns > STEP_NS) {
        mono_base_ns += ref_ns - clock_ns;
        real_base_ns += ref_ns - clock_ns;
        error = 0;
    } else {
        error = (uint32_t)(ahead ? clock_ns - ref_ns : ref_ns - clock_ns);
        if (error > SLEW_MAX_NS) {
            error = SLEW_MAX_NS;
        }
    }
    adjust = (uint32_t)(((uint64_t)mult_measured * error) >> SLEW_SHIFT);
    data->mult = ahead ? mult_measured - adjust : mult_measured + adjust;
    data->cycle_base = cycles;
    target = vclock_course_ns(mono_base_ns);
    if (target > real_base_ns + STEP_NS) {
        real_base_ns = target;
    }
    data->real_mult = vclock_real_rate(data->mult, (int64_t)(target - real_base_ns));
    vclock_write_times();
    vclock_write_end();
    spin_unlock(&vclock_lock);
}

int vclock_read(int real, int coarse, int64_t *sec, int64_t *nsec)
{
    uint64_t ns, cycles, delta;
    uint32_t seq;
    int64_t s;

    if (!running) {
        return 0;
    }
    do {
        while ((seq = data->seq) & 1u) {
            __asm__ __volatile__("pause");
        }
        __asm__ __volatile__("" : : : "memory");
        s = real ? data->real_sec : data->mono_sec;
        ns = real ? data->real_nsec : data->mono_nsec;
        if (!coarse) {
            cycles = tsc_ordered();
            delta = cycles >= data->cycle_base ? cycles - data->cycle_base : 0;
            ns += vclock_scale(delta, real ? data->real_mult : data->mult, data->shift);
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

int vclock_read_cycles(int real, uint64_t *ns, uint64_t *cycles)
{
    uint64_t base, c, delta;
    uint32_t seq;

    if (!running) {
        return 0;
    }
    do {
        while ((seq = data->seq) & 1u) {
            __asm__ __volatile__("pause");
        }
        __asm__ __volatile__("" : : : "memory");
        if (real) {
            base = (uint64_t)data->real_sec * NSEC_PER_SEC + data->real_nsec;
        } else {
            base = (uint64_t)data->mono_sec * NSEC_PER_SEC + data->mono_nsec;
        }
        c = tsc_ordered();
        delta = c >= data->cycle_base ? c - data->cycle_base : 0;
        base += vclock_scale(delta, real ? data->real_mult : data->mult, data->shift);
        __asm__ __volatile__("" : : : "memory");
    } while (data->seq != seq);
    *ns = base;
    *cycles = c;
    return 1;
}

void vclock_set_course(const struct rtc_course *c, int step)
{
    uint64_t cycles;
    uint32_t flags;

    if (!running) {
        return;
    }
    flags = spin_lock_irqsave(&vclock_lock);
    course = *c;
    vclock_write_begin();
    cycles = tsc_ordered();
    mono_base_ns = vclock_at(cycles);
    real_base_ns = step ? vclock_course_ns(mono_base_ns) : vclock_real_at(cycles);
    data->cycle_base = cycles;
    data->real_mult = vclock_real_rate(data->mult, 0);
    vclock_write_times();
    vclock_write_end();
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
