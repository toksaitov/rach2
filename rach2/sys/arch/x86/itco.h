/* The Intel version 2 TCO watchdog of ICH9 and Lynx Point-LP. */
#ifndef _ITCO_H_
#define _ITCO_H_

/** Probe once after board_init and paging, under the big lock. Register
 * /dev/watchdog only when the chipset and its registers answer: halted, or
 * running for the kernel to feed when firmware left it so or the line has
 * `rach2.itco.arm`. */
void itco_init(void);

/** Make the thread watchdogd, which feeds a running timer that no program
 * holds, as Linux's kthread worker of that name does. Called with the
 * platform's other threads, when the first program starts; a no-op when no
 * watchdog was registered or the thread runs. */
void itco_thread_start(void);

/** Count down to the next feed of a running timer that no program holds,
 * every half timeout, as Linux's watchdog core does before user space takes
 * over, and wake watchdogd to do it; during boot and in the kernel debug
 * shell, where the scheduler runs no thread, feed it here. From the boot
 * processor's tick, with interrupts off. The feed stops when the tick stops
 * or the threads stop running, so that a kernel stopped by a panic or a
 * hang is reset by the chipset. */
void itco_tick(void);

/** Halt a registered watchdog for an orderly stop, under the big lock. */
void itco_shutdown(void);

#endif
