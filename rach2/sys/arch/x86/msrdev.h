/* /dev/cpu/N/msr, each processor's model-specific registers (msrdev.c). */
#ifndef _MSRDEV_H_
#define _MSRDEV_H_

/**
 * Register /dev/cpu/N/msr for each processor online, as Linux's msr driver
 * makes them: major 202 and the processor's number, mode 0600, with its
 * device in /sys/class/msr; and /sys/module/msr. Called by
 * `platform_cpus_started` once the processors are started; a failure is
 * logged and the rest go on.
 */
void msrdev_start(void);

#endif /* _MSRDEV_H_ */
