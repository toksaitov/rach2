/*
 * refclock.c: the HPET's main counter or the ACPI PM timer, as refclock.h
 * describes.
 *
 * The HPET's registers (IA-PC HPET specification 1.0a, 2.3): the general
 * capabilities at offset 0, whose upper half is the counter's period in
 * femtoseconds; the general configuration at 0x10, whose bit 0 starts the
 * main counter and bit 1 routes the timers to the 8254's and the RTC's
 * lines; and the main counter at 0xF0, of which the low 32 bits are read.
 */

#include "refclock.h"

#include "../../klog.h"
#include "../../mm/paging.h"

#define HPET_CAPS_PERIOD 0x04 /* the upper half of the capabilities */
#define HPET_CONFIG      0x10
#define HPET_COUNTER     0xF0
#define HPET_ENABLE      0x1u
#define HPET_LEGACY      0x2u

/* The specification's bounds on the period: at most 100 ns, and anything
 * faster than a nanosecond is a register that does not say. */
#define HPET_PERIOD_MAX 100000000u
#define HPET_PERIOD_MIN 1000000u

#define PM_TIMER_HZ 3579545u

#define SOURCE_NONE 0
#define SOURCE_HPET 1
#define SOURCE_PM   2

static int source;
static volatile uint32_t *hpet;
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

int refclock_init(const struct acpi_info *a)
{
    source = SOURCE_NONE;
    if (a->hpet && (sizeof(uintptr_t) >= 8 || (a->hpet_address >> 32) == 0)) {
        uintptr_t va = map_mmio((uintptr_t)a->hpet_address, 0x400);
        uint32_t period;

        if (va != 0) {
            hpet = (volatile uint32_t *)va;
            period = hpet[HPET_CAPS_PERIOD / 4];
            if (period >= HPET_PERIOD_MIN && period <= HPET_PERIOD_MAX) {
                hpet[HPET_CONFIG / 4] = (hpet[HPET_CONFIG / 4] & ~HPET_LEGACY) | HPET_ENABLE;
                source = SOURCE_HPET;
                hz = hpet_hz(period);
                mask = 0xFFFFFFFFu;
                klog(KLOG_INFO, "hpet", "ready: %d Hz, period %d fs", (int)hz, (int)period);
                return 0;
            }
            klog(KLOG_WARN, "hpet", "refused: a period of %d fs", (int)period);
        }
    }
    if (a->fadt && a->pm_timer.address != 0) {
        uint32_t first, now;
        int i;

        pm = a->pm_timer;
        mask = a->pm_timer_bits == 32 ? 0xFFFFFFFFu : 0xFFFFFFu;
        if (acpi_gas_read(&pm, &first) == 0) {
            /* A timer that reads all ones or never moves is not there. */
            now = first;
            for (i = 0; i < 100000 && (now & mask) == (first & mask); i++) {
                (void)acpi_gas_read(&pm, &now);
            }
            if ((now & mask) != (first & mask) && (now & mask) != mask) {
                source = SOURCE_PM;
                hz = PM_TIMER_HZ;
                klog(KLOG_INFO, "pmtimer", "ready: %d-bit at 0x%x", a->pm_timer_bits,
                     (uint32_t)a->pm_timer.address);
                return 0;
            }
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
    uint32_t v = 0;

    if (source == SOURCE_HPET) {
        return hpet[HPET_COUNTER / 4];
    }
    if (source == SOURCE_PM) {
        (void)acpi_gas_read(&pm, &v);
    }
    return v & mask;
}

/**
 * Wait `usec` microseconds, a millisecond of counts at a time, so that the
 * counts of one step stay inside 32 bits and below the PM timer's 24-bit
 * wrap of 4.7 seconds.
 */
void refclock_wait_us(uint32_t usec)
{
    if (source == SOURCE_NONE) {
        return;
    }
    while (usec > 0) {
        uint32_t step = usec > 1000u ? 1000u : usec;
        uint32_t want = refclock_scale(step, hz, 1000000u);
        uint32_t start = refclock_read();

        while (((refclock_read() - start) & mask) < want) {
            /* the counter is the clock */
        }
        usec -= step;
    }
}
