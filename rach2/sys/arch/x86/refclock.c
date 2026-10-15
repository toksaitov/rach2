/*
 * refclock.c: the HPET's main counter or the ACPI PM timer, as refclock.h
 * describes.
 *
 * The HPET's registers (IA-PC HPET specification 1.0a, 2.3): the general
 * capabilities at offset 0, whose bits 8 to 12 are the number of the last
 * comparator and whose upper half is the counter's period in femtoseconds;
 * the general configuration at 0x10, whose bit 0 starts the main counter and
 * bit 1 routes the timers to the 8254's and the RTC's lines; the interrupt
 * status at 0x20, whose bits a write of ones clears; the main counter at
 * 0xF0, of which the low 32 bits are read; and each comparator's
 * configuration at 0x100 plus 0x20 for each, whose bit 2 lets it interrupt
 * and bit 14 sends that interrupt as a message.
 */

#include "refclock.h"

#include "../../klog.h"
#include "../../mm/paging.h"

#define HPET_CAPS        0x00
#define HPET_CAPS_PERIOD 0x04 /* the upper half of the capabilities */
#define HPET_CONFIG      0x10
#define HPET_STATUS      0x20
#define HPET_COUNTER     0xF0
#define HPET_TIMER(n)    (0x100 + 0x20 * (n))
#define HPET_ENABLE      0x1u
#define HPET_LEGACY      0x2u
#define HPET_TIMER_INT   0x4u
#define HPET_TIMER_FSB   0x4000u

/* The registers mapped, and so the comparators whose configuration lies in
 * them, 24 of the specification's 32. */
#define HPET_MAP_BYTES  0x400u
#define HPET_TIMERS_MAX ((HPET_MAP_BYTES - 0x100u) / 0x20u)

/* The reads in a row that may find the counter where it was before it is
 * called stopped, at the start and in a wait, as udelay.c bounds the 8254's
 * wait: a running HPET or PM timer moves between any two reads. */
#define STALL_READS 100000u

/* The specification's bounds on the period: at most 100 ns, and anything
 * faster than a nanosecond is a register that does not say. */
#define HPET_PERIOD_MAX 100000000u
#define HPET_PERIOD_MIN 1000000u

#define PM_TIMER_HZ 3579545u

#define SOURCE_NONE 0
#define SOURCE_HPET 1
#define SOURCE_PM   2

/* The counter `refclock_init` took, SOURCE_NONE before; the HPET's
 * registers, and a host test's in their place; the counts a second; the
 * counter's bits; and the PM timer's register. */
static int source;
static volatile uint32_t *hpet;
static volatile uint32_t *test_hpet;
static uint32_t hz;
static uint32_t mask;
static struct acpi_gas pm;

uint32_t refclock_scale(uint32_t a, uint32_t b, uint32_t c)
{
#if defined(__x86_64__) || defined(RACH2_HOSTTEST)
    return (uint32_t)((uint64_t)a * b / c);
#else
    uint64_t product = (uint64_t)a * b;
    uint32_t q, r;

    /* `divl` divides edx:eax, where a division in C would call libgcc. */
    __asm__("divl %4"
            : "=a"(q), "=d"(r)
            : "0"((uint32_t)product), "1"((uint32_t)(product >> 32)), "rm"(c)
            : "cc");
    (void)r;
    return q;
#endif
}

/** Femtoseconds a count into counts a second: 10^15 over the period, as
 * 10^9 times (10^6 over the period). */
static uint32_t hpet_hz(uint32_t period_fs)
{
    return refclock_scale(1000000000u, 1000000u, period_fs);
}

/** Whether the counter `read` gives moves within STALL_READS reads, and
 * reads other than all ones in `mask`. */
static int counter_runs(uint32_t (*read)(void))
{
    uint32_t first = read() & mask, now = first;
    uint32_t i;

    for (i = 0; i < STALL_READS && now == first; i++) {
        now = read() & mask;
    }
    return now != first && now != mask;
}

/** The low 32 bits of the HPET's main counter. */
static uint32_t hpet_read(void)
{
    return hpet[HPET_COUNTER / 4];
}

/** The PM timer's register as read, 0 when the read fails. */
static uint32_t pm_read(void)
{
    uint32_t v = 0;

    (void)acpi_gas_read(&pm, &v);
    return v;
}

/**
 * Take the HPET as the counter: its comparators' interrupts off, each as a
 * line and as a message, and the status of any already raised cleared, so
 * that nothing a firmware left armed asserts a line no handler here clears;
 * then the legacy routing off and the counter started. Returns 0, or -1 for
 * a counter that does not move.
 */
static int hpet_take(void)
{
    uint32_t n = ((hpet[HPET_CAPS / 4] >> 8) & 0x1Fu) + 1u, i;

    if (n > HPET_TIMERS_MAX) {
        n = HPET_TIMERS_MAX;
    }
    for (i = 0; i < n; i++) {
        hpet[HPET_TIMER(i) / 4] &= ~(HPET_TIMER_INT | HPET_TIMER_FSB);
    }
    hpet[HPET_STATUS / 4] = 0xFFFFFFFFu;
    hpet[HPET_CONFIG / 4] = (hpet[HPET_CONFIG / 4] & ~HPET_LEGACY) | HPET_ENABLE;
    mask = 0xFFFFFFFFu;
    return counter_runs(hpet_read) ? 0 : -1;
}

int refclock_init(const struct acpi_info *a)
{
    source = SOURCE_NONE;
    if (a->hpet && (sizeof(uintptr_t) >= 8 || (a->hpet_address >> 32) == 0)) {
        uintptr_t va = test_hpet ? 1u : map_mmio((uintptr_t)a->hpet_address, HPET_MAP_BYTES);
        uint32_t period;

        if (va != 0) {
            hpet = test_hpet ? test_hpet : (volatile uint32_t *)va;
            period = hpet[HPET_CAPS_PERIOD / 4];
            if (period < HPET_PERIOD_MIN || period > HPET_PERIOD_MAX) {
                klog(KLOG_WARN, "hpet", "refused: a period of %d fs", (int)period);
            } else if (hpet_take() != 0) {
                klog(KLOG_WARN, "hpet", "refused: the counter does not run");
            } else {
                source = SOURCE_HPET;
                hz = hpet_hz(period);
                klog(KLOG_INFO, "hpet", "ready: %d Hz, period %d fs", (int)hz, (int)period);
                return 0;
            }
        }
    }
    if (a->fadt && a->pm_timer.address != 0) {
        uint32_t first;

        pm = a->pm_timer;
        mask = a->pm_timer_bits == 32 ? 0xFFFFFFFFu : 0xFFFFFFu;
        /* A timer that reads all ones or never moves is not there. */
        if (acpi_gas_read(&pm, &first) == 0 && counter_runs(pm_read)) {
            source = SOURCE_PM;
            hz = PM_TIMER_HZ;
            klog(KLOG_INFO, "pmtimer", "ready: %d-bit at 0x%x", a->pm_timer_bits,
                 (uint32_t)a->pm_timer.address);
            return 0;
        }
        klog(KLOG_WARN, "pmtimer", "refused: the counter does not run");
    }
    return -1;
}

const char *refclock_name(void)
{
    return source == SOURCE_HPET ? "hpet" : source == SOURCE_PM ? "pm timer" : 0;
}

uint32_t refclock_hz(void)
{
    return hz;
}

uint32_t refclock_mask(void)
{
    return mask;
}

uint32_t refclock_read(void)
{
    if (source == SOURCE_HPET) {
        return hpet_read();
    }
    if (source == SOURCE_PM) {
        return pm_read() & mask;
    }
    return 0;
}

void refclock_set_registers(volatile uint32_t *regs)
{
    test_hpet = regs;
}

uint32_t refclock_counts_at_least(uint32_t usec)
{
    uint32_t want = refclock_scale(usec, hz, 1000000u);

    if ((uint64_t)want * 1000000u < (uint64_t)usec * hz) {
        want++;
    }
    return want + 1u;
}

/**
 * Wait `usec` microseconds, a millisecond of counts at a time, so that the
 * counts of one step stay inside 32 bits and below the PM timer's 24-bit
 * wrap of 4.7 seconds. Each step waits the counts that cover it at least
 * (`refclock_counts_at_least`). A counter that stops for STALL_READS reads
 * ends the wait, which is bounded as `k_udelay` promises.
 */
void refclock_wait_us(uint32_t usec)
{
    if (source == SOURCE_NONE) {
        return;
    }
    while (usec > 0) {
        uint32_t step = usec > 1000u ? 1000u : usec;
        uint32_t want = refclock_counts_at_least(step);
        uint32_t start = refclock_read(), last = start, now, still = 0;

        while ((((now = refclock_read()) - start) & mask) < want) {
            if (now != last) {
                last = now;
                still = 0;
            } else if (++still == STALL_READS) {
                return;
            }
        }
        usec -= step;
    }
}
