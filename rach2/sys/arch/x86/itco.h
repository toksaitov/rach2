/* The Intel version 2 TCO watchdog of ICH9 and Lynx Point-LP. */
#ifndef _ITCO_H_
#define _ITCO_H_

/** Probe once after board_init and paging, under the big lock. Register
 * /dev/watchdog only when the chipset and its registers answer: halted, or
 * running for the kernel to feed when firmware left it so or the line has
 * `rach2.itco.arm`. */
void itco_init(void);

/** Feed a running timer that no program holds, every half timeout, as
 * Linux's watchdog core does before user space takes over. From the boot
 * processor's tick, with interrupts off; it stops with the tick, so that a
 * kernel stopped by a panic or a hang is reset by the chipset. */
void itco_tick(void);

/** Halt a registered watchdog for an orderly stop, under the big lock. */
void itco_shutdown(void);

#endif
