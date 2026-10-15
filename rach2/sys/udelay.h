/*
 * udelay.h: the short waits device bring-up sequences require, measured
 * against a hardware counter of known rate.
 */
#ifndef _UDELAY_H_
#define _UDELAY_H_

#include "lib/kint.h"

/**
 * Wait at least `usec` microseconds, spinning.
 *
 * The wait is measured against a counter, since a count of instructions
 * lasts as long as the processor's speed makes it. On a PC the counter is
 * the 8254's channel 0, latched and read without disturbing the tick, and on
 * a machine with no 8254 the HPET or the ACPI PM timer, which the platform
 * selects (`udelay_wait`, arch/x86/pit.h); arch/x86/udelay.c gives the mode
 * 3 counting rule its conversion depends on. It needs no interrupts and may
 * run with them off. A counter that stops moving ends the wait early, so it
 * returns on a machine whose counter is absent.
 */
void k_udelay(uint32_t usec);

#endif /* _UDELAY_H_ */
