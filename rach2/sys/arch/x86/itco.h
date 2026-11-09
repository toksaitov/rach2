/* The Intel version 2 TCO watchdog of ICH9 and Lynx Point-LP. */
#ifndef _ITCO_H_
#define _ITCO_H_

/** Probe once after board_init and paging, under the big lock. Register
 * /dev/watchdog, halted, only when the chipset and its registers answer. */
void itco_init(void);

/** Halt a registered watchdog for an orderly stop, under the big lock. */
void itco_shutdown(void);

#endif
