/* printk.h: the printk module's parameters, as Linux has them on the boot
 * line and in /sys/module/printk/parameters. */
#ifndef _PRINTK_H_
#define _PRINTK_H_

/**
 * Set the parameters from the boot line, as Linux reads them: the early
 * `ignore_loglevel` and the setup `no_console_suspend`, then each
 * `printk.NAME=` boolean, a bare `printk.NAME` true, the last that gives a
 * boolean counting. Called once the line is captured, before the boot's
 * records go out, so that they follow `printk.time` and `ignore_loglevel`.
 */
void printk_params_early(void);

/**
 * Make /sys/module/printk/parameters with its five files, each `Y` or `N`
 * and a newline, mode 0644, which root writes with any text Linux's
 * `kstrtobool` takes and which answer -EINVAL to another. A failure is
 * logged and leaves the parameters as they are.
 */
void printk_params_start(void);

#endif /* _PRINTK_H_ */
