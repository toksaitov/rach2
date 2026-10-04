/*
 * refclock_hosttest.c: the reference counter's choice of the HPET or the PM
 * timer, and its waits.
 *
 * host-deps: rach2/sys/arch/x86/refclock.c
 *
 * The HPET's registers are an array here, given to the reader through
 * `refclock_set_registers`, and the PM timer is a counter that moves by a
 * count at each read until a row stops it. A firmware that left the HPET's
 * comparators armed must find them quiet and their status cleared, whatever
 * else happens; an HPET whose counter stands still, as one in an array
 * does, must be refused for the PM timer; a PM timer that stands still must
 * be refused too; a wait must last the counts that cover its time at least;
 * and a wait on a counter that stops must end.
 */

#include <stdarg.h>

extern int printf(const char *fmt, ...);

#include "../../rach2/sys/arch/x86/refclock.h"
#include "../../rach2/sys/mm/paging.h"

static int fails;

#define CHECK(cond, what)                   \
    do {                                    \
        if (!(cond)) {                      \
            printf("  FAIL: %s\n", (what)); \
            fails++;                        \
        }                                   \
    } while (0)

void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

uintptr_t map_mmio(uintptr_t phys, size_t size)
{
    (void)phys;
    (void)size;
    CHECK(0, "map_mmio is not asked for the HPET a test gives");
    return 0;
}

/* The PM timer: its value, the reads it moves on, and the reads made. */
static uint32_t pm_value, pm_moves_for;
static unsigned long pm_reads;

int acpi_gas_read(const struct acpi_gas *g, uint32_t *value)
{
    (void)g;
    pm_reads++;
    if (pm_moves_for > 0) {
        pm_moves_for--;
        pm_value++;
    }
    *value = pm_value;
    return 0;
}

static uint32_t regs[0x400 / 4];

int main(void)
{
    struct acpi_info a;
    unsigned int i;

    printf("refclock\n");
    for (i = 0; i < sizeof(a); i++) {
        ((unsigned char *)&a)[i] = 0;
    }
    a.hpet = 1;
    a.hpet_address = 0xFED00000u;
    a.fadt = 1;
    a.pm_timer.space = ACPI_SPACE_IO;
    a.pm_timer.address = 0x408;
    a.pm_timer_bits = 24;
    regs[0x00 / 4] = 2u << 8;   /* three comparators */
    regs[0x04 / 4] = 69841279u; /* 14.318 MHz */
    regs[0x10 / 4] = 0x2u;      /* legacy routing on, the counter off */
    regs[0x20 / 4] = 0x5u;      /* comparators 0 and 2 raised */
    regs[0x100 / 4] = 0x4u | 0x2u;
    regs[0x120 / 4] = 0x4000u | 0x4u;
    regs[0x140 / 4] = 0x4u;
    pm_moves_for = 1000000u;
    refclock_set_registers(regs);
    CHECK(refclock_init(&a) == 0, "a reference counter is found");
    CHECK((regs[0x100 / 4] & 0x4004u) == 0 && (regs[0x120 / 4] & 0x4004u) == 0 &&
              (regs[0x140 / 4] & 0x4004u) == 0,
          "every comparator's interrupt is off, as a line and as a message");
    CHECK(regs[0x100 / 4] == 0x2u, "and a comparator keeps its other bits");
    CHECK(regs[0x20 / 4] == 0xFFFFFFFFu, "the raised status is cleared by a write of ones");
    CHECK((regs[0x10 / 4] & 0x3u) == 0x1u, "the legacy routing is off and the counter on");
    CHECK(refclock_name() != 0 && refclock_name()[0] == 'p' && refclock_hz() == 3579545u,
          "an HPET whose counter stands still gives way to the PM timer");

    CHECK(refclock_counts_at_least(200) == 717u, "200 us of the PM timer are 717 counts");
    CHECK(refclock_counts_at_least(1) == 5u, "1 us of it is 5 counts");
    CHECK(refclock_counts_at_least(1000) == 3581u, "1000 us of it are 3581 counts");

    pm_value = 100;
    pm_moves_for = 1000000u;
    refclock_wait_us(200);
    CHECK(pm_value >= 100u + 717u, "a wait of 200 us lasts 717 counts at least");

    pm_moves_for = 10;
    pm_reads = 0;
    refclock_wait_us(1000);
    CHECK(pm_reads < 200000ul, "a wait on a counter that stops ends");

    regs[0x00 / 4] = 0;
    pm_moves_for = 0;
    CHECK(refclock_init(&a) == -1 && refclock_name() == 0,
          "with both counters standing still there is none");

    if (fails) {
        printf("refclock_hosttest: %d check(s) failed\n", fails);
        return 1;
    }
    printf("refclock_hosttest: all checks passed\n");
    return 0;
}
