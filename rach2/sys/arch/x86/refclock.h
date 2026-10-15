#ifndef _REFCLOCK_H_
#define _REFCLOCK_H_

#include "acpi.h"

#include "../../lib/kint.h"

/*
 * refclock: a counter that runs at a rate the firmware states, for the
 * local APIC timer to be measured against and, on a machine with no 8254,
 * for short waits. The HPET's main counter when the HPET table gives one,
 * and the ACPI PM timer, 3.579545 MHz, when it does not.
 *
 * Neither interrupts anything; both are read. The HPET is enabled here,
 * with its legacy replacement left off, so the 8254 and the RTC keep their
 * lines, and with every comparator's interrupt off.
 */

/**
 * Find the counter in the tables `a` describes: 0 when one is ready, -1
 * when the machine has neither. An HPET whose period is out of the
 * specification's bounds, or a counter that does not move, is refused with
 * the reason in the log. Called once, at boot, from
 * `platform_firmware_init`.
 */
int refclock_init(const struct acpi_info *a);

/** "hpet" or "pm timer", or NULL before `refclock_init` found one. */
const char *refclock_name(void);

/** The counts a second of the counter `refclock_init` found. */
uint32_t refclock_hz(void);

/** The counter now, in its own width, 0 when there is none: `refclock_mask`
 * has its bits. */
uint32_t refclock_read(void);
uint32_t refclock_mask(void);

/**
 * The counts that cover `usec` microseconds at least, whatever fraction of a
 * count had gone when the first was read: the quotient rounded up, and one
 * more.
 */
uint32_t refclock_counts_at_least(uint32_t usec);

/** Wait at least `usec` microseconds by the counter; a counter that stops
 * ends the wait. */
void refclock_wait_us(uint32_t usec);

/** For a host test: the HPET's registers at `regs`, which `map_mmio`
 * cannot give a host program, in place of the table's address; NULL maps
 * the table's again. */
void refclock_set_registers(volatile uint32_t *regs);

/** `a` times `b` divided by `c`, through a 64-bit product, for a quotient
 * the caller knows fits in 32 bits. */
uint32_t refclock_scale(uint32_t a, uint32_t b, uint32_t c);

#endif /* _REFCLOCK_H_ */
