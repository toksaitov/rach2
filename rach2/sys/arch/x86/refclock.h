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
 * lines.
 */

/** Find the counter: 0 when one is ready, -1 when the machine has neither. */
int refclock_init(const struct acpi_info *a);

/** "hpet" or "pm timer", or NULL before `refclock_init` found one. */
const char *refclock_name(void);

/** The counts a second. */
uint32_t refclock_hz(void);

/** The counter now, in its own width: `refclock_mask` has its bits. */
uint32_t refclock_read(void);
uint32_t refclock_mask(void);

/** Wait `usec` microseconds by the counter. */
void refclock_wait_us(uint32_t usec);

/** `a` times `b` divided by `c`, through a 64-bit product, for a quotient
 * the caller knows fits in 32 bits. */
uint32_t refclock_scale(uint32_t a, uint32_t b, uint32_t c);

#endif /* _REFCLOCK_H_ */
